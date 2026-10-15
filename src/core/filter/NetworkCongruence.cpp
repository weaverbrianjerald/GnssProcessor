/**
 * @file NetworkCongruence.cpp
 * @brief Station congruence screen: the per-station coordinate sweep and its
 *        emission (docs/SPEC_redundancy_census.md CG-INC-1..3).  See
 *        NetworkCongruence.hpp for the design argument, the sign convention and
 *        the two @f$\hat\sigma_0@f$ corrections taken from the block card.
 *
 * Measure-only.  Nothing here writes a weight, a state, a coordinate or a
 * decision; this translation unit reads the solved system and emits counters,
 * ladders, a per-station MDB table and events.  SPEC §8 row 1 assigns any
 * response (a deweight, an exclusion, a re-coordination) to a separate card,
 * because re-coordinating a station is a datum act with two edges to bound under
 * standing invariant #7.
 *
 * ## Cost
 *
 * The sweep runs inside the redundancy census's swept branch, so it inherits that
 * card's stride, targeted-escalation and trip schedule and adds no scheduler
 * (SPEC §3).  Per swept station it forms one @f$b\times b@f$ residual cofactor
 * over the station's data rows, one @f$b\times 3@f$ contraction and one
 * @f$3\times 3@f$ eigensolve.  The cofactor and the @c rowRedundancy call the
 * census already made for the same station are dominated by the same two terms
 * (@f$N_{rr}^{-1}N_{sr}^\top@f$ and an @f$n_{shared}^2@f$ Gram), which keeps
 * SPEC §4's budget ("≤ 2× the redundancy sweep it rides") within reach.  The
 * budget is checked, not assumed: @c tallies.wall_ms is measured here.
 *
 * The post-fit residual is not recomputed here: the census's rider block forms it
 * once and passes it to both this screen and the block census.
 */

#include "filter/NetworkCongruence.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "logging/NetworkRunTraces.hpp"

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkSetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunTypes.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

// ===========================================================================
//  The per-station congruence sweep (INC-1)
// ===========================================================================

void networkCongruenceSweepStation(NetworkCongruence& acc,
                                   const NetworkNormalEquations& kernel,
                                   std::size_t slot, int station_index,
                                   const StationAssembly& R,
                                   const std::vector<NetRowClass>& cls,
                                   const Eigen::VectorXd& resid,
                                   const Eigen::MatrixXd& cov_free,
                                   double delta_0, bool datum_changed,
                                   bool block_known, bool block_flagged,
                                   NetworkCongruenceTallies& tallies,
                                   std::vector<CongruenceHit>& hits) {
    if (!acc.enabled()) return;
    const Eigen::Index m = R.y.size();
    if (m <= 0 || resid.size() != m
        || static_cast<Eigen::Index>(cls.size()) < m) return;

    const auto t0 = std::chrono::steady_clock::now();
    ++tallies.station_sweeps;

    // ---- which rows carry position information -----------------------------
    // Observation rows only (code and phase), and only those with a nonzero,
    // finite geometry triple, a positive weight and a finite residual.  Pins and
    // coupling rows are excluded for the reason the block card excludes them: a
    // pin's ~1e10 weight would swamp anything it joined, and on a within-epoch
    // re-solve the coupling rows are re-whitened in a copy and no longer describe
    // the system that was solved.  SH↔slant tie rows are excluded because they
    // have no position dependence; their partial is exactly zero and would only
    // add null rows to the contraction.
    //
    // `row < R.row_pos_enu.rows()` is a structural bound: every fold after the
    // split appends rows, so only the leading block can carry geometry.  A station
    // assembled before the screen was armed (or one whose position column was
    // missing) has an empty row_pos_enu; its rows are counted in no_geom_rows
    // rather than read as "no fault here".
    const Eigen::Index m_geo = R.row_pos_enu.rows();
    std::vector<Eigen::Index> rows;
    rows.reserve(static_cast<std::size_t>(m));
    int n_code = 0, n_phase = 0;
    long long dropped_geom = 0;
    for (Eigen::Index row = 0; row < m; ++row) {
        const NetRowClass k = cls[static_cast<std::size_t>(row)];
        if (k != NetRowClass::ObsCode && k != NetRowClass::ObsPhase) continue;
        if (row >= m_geo || R.row_pos_enu.cols() != 3) { ++dropped_geom; continue; }
        const double w_i = (row < R.w.size()) ? R.w(row) : 0.0;
        if (!(w_i > 0.0) || !std::isfinite(w_i)) { ++dropped_geom; continue; }
        if (!std::isfinite(resid(row)))          { ++dropped_geom; continue; }
        const Eigen::Vector3d h = R.row_pos_enu.row(row).transpose();
        if (!h.allFinite() || h.isZero(0.0))     { ++dropped_geom; continue; }
        rows.push_back(row);
        if (k == NetRowClass::ObsCode) ++n_code; else ++n_phase;
    }
    tallies.no_geom_rows += dropped_geom;
    if (rows.empty()) {
        ++tallies.no_rows;
        tallies.wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }

    const Eigen::Index b = static_cast<Eigen::Index>(rows.size());
    Eigen::MatrixXd Bs(b, R.A_s.cols()), Br(b, R.A_r.cols()), Hd(b, 3);
    Eigen::VectorXd bw(b), be(b);
    for (Eigen::Index i = 0; i < b; ++i) {
        const Eigen::Index row = rows[static_cast<std::size_t>(i)];
        Bs.row(i) = R.A_s.row(row);
        Br.row(i) = R.A_r.row(row);
        bw(i) = R.w(row);
        be(i) = resid(row);
        // Sign: row_pos_enu is dy/d(Delta) with Delta = p_true - p_held (em.H's
        // own position partial).  The reported quantity is the held coordinate's
        // error d = p_held - p_true = -Delta, so H_B = -row_pos_enu, and a +5 cm
        // East perturbation of a held coordinate reads delta_E = +5 cm (SPEC 6.1 /
        // 7.2).  M, T and every MDB are invariant to this sign; only delta depends
        // on it.  See the sign section of NetworkCongruence.hpp.
        Hd.row(i) = -R.row_pos_enu.row(row);
    }

    Eigen::MatrixXd Q;
    try { Q = kernel.blockResidualCofactor(slot, Bs, Br, bw, cov_free); }
    catch (...) {
        ++tallies.solve_failed;
        tallies.wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }
    if (Q.rows() != b) {
        ++tallies.solve_failed;
        tallies.wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }

    // Q is used forward only (H'P Q P H) and never inverted, so a rank-deficient
    // block is admissible and needs no uncheckable category.  The degeneracy that
    // matters appears in the 3x3 eigenproblem, where it is named per direction
    // rather than pooled into one yes/no.
    NetworkCongruence::Input in;
    in.station = station_index;
    in.solve   = congruenceStatistic(Hd, bw, Q, be, delta_0);
    in.n_code  = n_code;
    in.n_phase = n_phase;
    in.label   = datum_changed ? CongruenceLabel::Frame : CongruenceLabel::Clean;
    in.block_known   = block_known;
    in.block_flagged = block_flagged;

    const NetworkCongruence::StationStat st = acc.addStation(in);
    if (!st.accepted) {
        ++tallies.solve_failed;
        tallies.wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }
    tallies.rows += b;
    if (st.degenerate) ++tallies.degenerate;
    if (st.reported)   ++tallies.reported;

    CongruenceHit h;
    h.station = station_index;
    h.solve   = in.solve;
    h.n_code = n_code; h.n_phase = n_phase;
    h.t = st.t; h.sigma0 = st.sigma0;
    h.raw_formal = st.raw_formal;
    h.reported   = st.reported;
    h.null_geom  = st.null_geom;
    h.label = in.label;
    h.block_known = block_known;
    h.block_flagged = block_flagged;
    hits.push_back(std::move(h));

    if (st.t > tallies.max_t) {
        tallies.max_t = st.t;
        tallies.max_id = fmt::format(
            "#{} d=({:.4f},{:.4f},{:.4f}) m ENU q={} @tow {:.1f}", station_index,
            in.solve.delta(0), in.solve.delta(1), in.solve.delta(2),
            in.solve.dof, acc.lastTow());
    }

    tallies.wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ===========================================================================
//  Emission
// ===========================================================================

namespace {

/// Station name for an identity string, or a positional fallback ("#<idx>", or
/// "(network)" for a negative index), so a finding without a name is still reported.
std::string stationName(const std::vector<NetStation>& stations, int idx) {
    if (idx < 0 || idx >= static_cast<int>(stations.size()))
        return (idx < 0) ? std::string("(network)") : fmt::format("#{}", idx);
    return stations[static_cast<std::size_t>(idx)].name;
}

/// A metre-valued cell with three distinct states:
///   * a number: the value in metres (an MDB, for example);
///   * `inf`: the direction is not estimable; a category, never a large number a
///     consumer could act on (@ref BaardaReliability.hpp's convention);
///   * `n/a` (NaN input): there was no sample to summarize.
/// The last two stay separate: "the screen never had a usable epoch here" and
/// "this direction is unobservable" are different findings (invariant #4).
std::string mCell(double v) {
    if (std::isnan(v)) return std::string("n/a");
    return std::isfinite(v) ? fmt::format("{:.6g}", v) : std::string("inf");
}

}  // namespace

void networkCongruenceEmit(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const std::vector<NetStation>& stations = run.setup.stations;
    // ---- End of the bindings ----
    (void)config_;
    (void)log;
    NetworkCongruence& acc = ctx.carry.cg;
    NetworkCongruenceTallies& t = ctx.counters.cg;
    if (!acc.enabled()) return;

    const auto t0 = std::chrono::steady_clock::now();
    ++t.passes;
    if (acc.epochStations() > 0) ++t.epochs;

    // ---- one row per swept station ----------------------------------------
    // The block census writes per-epoch rows per (family x label) ladder; this
    // screen's primary product is the per-station record instead: SPEC §7.2
    // grades a per-station run-median of delta_E, and §7.3's MDB table is a
    // per-station, per-direction quantity.  Neither survives aggregation, so the
    // trace carries each station-epoch, and the ladders are a run-end readout.
    if (traces.congruence_trace.is_open()) {
        for (const CongruenceHit& h : ctx.carry.cg_hits) {
            auto& o = traces.congruence_trace;
            o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << stationName(stations, h.station) << ','
              << congruenceLabelName(h.label) << ','
              << h.solve.b << ',' << h.n_code << ',' << h.n_phase << ','
              << h.solve.dof << ',' << (h.null_geom ? 1 : 0) << ','
              << fmt::format("{:.6g}", h.solve.delta(0)) << ','
              << fmt::format("{:.6g}", h.solve.delta(1)) << ','
              << fmt::format("{:.6g}", h.solve.delta(2)) << ','
              << fmt::format("{:.6g}", h.solve.T_raw) << ','
              << fmt::format("{:.6g}", h.t) << ','
              << fmt::format("{:.6g}", h.sigma0) << ','
              << (h.raw_formal ? 1 : 0) << ','
              << (acc.sigma0Warm() ? 1 : 0) << ','
              << fmt::format("{:.6g}", acc.sigma0Neff()) << ','
              << mCell(h.solve.mdb(0)) << ',' << mCell(h.solve.mdb(1)) << ','
              << mCell(h.solve.mdb(2)) << ','
              << mCell(h.solve.mdb(0) * h.sigma0) << ','
              << mCell(h.solve.mdb(1) * h.sigma0) << ','
              << mCell(h.solve.mdb(2) * h.sigma0) << ','
              << fmt::format("{:.6g}", h.solve.eval(0)) << ','
              << fmt::format("{:.6g}", h.solve.eval(2)) << ','
              << fmt::format("{:.4f}", h.solve.null_dir(0)) << ','
              << fmt::format("{:.4f}", h.solve.null_dir(1)) << ','
              << fmt::format("{:.4f}", h.solve.null_dir(2)) << ','
              << (h.block_known ? (h.block_flagged ? 1 : 0) : -1) << ','
              << (h.reported ? 1 : 0) << '\n';
        }
    }

    // ---- identities of reported stations, rate-limited ---------------------
    const long long now_ep = acc.epochs();
    const long long holdoff = ctx.knobs.cg_event_holdoff;
    for (const CongruenceHit& h : ctx.carry.cg_hits) {
        if (!h.reported) continue;
        const std::string sname = stationName(stations, h.station);
        auto it = ctx.carry.cg_event_last.find(sname);
        if (it != ctx.carry.cg_event_last.end() && now_ep - it->second < holdoff)
            continue;
        ctx.carry.cg_event_last[sname] = now_ep;
        // The statistic goes in `detail` as well as in the typed column, because
        // emitEvent formats statistic and threshold with a fixed 6 decimals, which
        // prints a small t_i as 0.000000.
        traces.emitEvent(
            se.time, "CONGRUENCE", "coordinate_offset", "", sname, h.t,
            acc.reportSigma(), "observe",
            fmt::format("t={:.6g};T={:.6g};d_e={:.4f};d_n={:.4f};d_u={:.4f};"
                        "mdb_e={};mdb_n={};mdb_u={};b={};n_code={};n_phase={};"
                        "q={};sigma0={:.6g};label={};block_flagged={};raw_formal={}",
                        h.t, h.solve.T_raw, h.solve.delta(0), h.solve.delta(1),
                        h.solve.delta(2), mCell(h.solve.mdb(0)),
                        mCell(h.solve.mdb(1)), mCell(h.solve.mdb(2)), h.solve.b,
                        h.n_code, h.n_phase, h.solve.dof, h.sigma0,
                        congruenceLabelName(h.label),
                        h.block_known ? (h.block_flagged ? "1" : "0") : "unknown",
                        h.raw_formal ? 1 : 0));
        ++t.events;
    }
    ctx.carry.cg_hits.clear();

    t.wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ===========================================================================
//  Run-end readout (standing invariant #4)
// ===========================================================================

void networkCongruenceRunEnd(const NetworkRunContext& ctx,
                             const std::shared_ptr<spdlog::logger>& log,
                             const std::vector<NetStation>& stations) {
    const NetworkCongruence& acc = ctx.carry.cg;
    const NetworkCongruenceTallies& t = ctx.counters.cg;
    if (!acc.enabled()) return;

    // Denominators first: this line separates "the screen ran and found nothing"
    // from "the screen never ran".
    log->info("Network (station congruence): {} emission pass(es), {} with a swept "
              "station, {} station-sweep(s) over {} data row(s) ({} code / {} phase); "
              "{} row(s) without usable geometry, {} station(s) with no post-fit "
              "residual, {} with no geometric row at all, {} whose 3x3 information "
              "yielded no statistic; {} degenerate (q < 3), {} reported, {} event(s); "
              "{:.1f} ms.",
              t.passes, t.epochs, t.station_sweeps, t.rows, acc.runCodeRows(),
              acc.runPhaseRows(), t.no_geom_rows, t.no_resid, t.no_rows,
              t.solve_failed, t.degenerate, t.reported, t.events, t.wall_ms);

    if (t.station_sweeps == 0) {
        log->info("Network (station congruence): ARMED but NO station sweep ever ran "
                  "— the redundancy census's stride/escalation never selected a station "
                  "(or every one was skipped).  This is a WIRING reading, not a finding.");
        return;
    }
    if (t.rows == 0) {
        log->info("Network (station congruence): {} station-sweep(s) contracted ZERO "
                  "rows.  Every observation row was dropped for want of a position "
                  "partial ({} such rows), so StationAssembly::row_pos_enu was never "
                  "populated — the INC-0 retention is not wired for this run.  NOT a "
                  "finding about the coordinates.",
                  t.station_sweeps, t.no_geom_rows);
        return;
    }

    log->info("Network (station congruence): sigma0 = {:.4g} ({}, n_eff = {:.1f}).  "
              "ONE network-wide family by design — a per-station sigma0 would let a "
              "faulty station calibrate its own fault away.  It is a SCALE, not a "
              "test, and deliberately unclamped: below 1 means the station-epochs are "
              "QUIETER than the model says and every t below is inflated accordingly.",
              acc.sigma0(), acc.sigma0Warm() ? "warm" : "WARM-UP", acc.sigma0Neff());

    // ---- the MDB table (SPEC §7.3's service deliverable) -------------------
    // The smallest coordinate fault this network can see, per station and per
    // direction, next to the run-median offset it measured.  On a carry_invariant
    // lineage the Up column is expected to be large or infinite; SPEC §10 rules
    // out tuning that away, so it is printed as measured.
    log->info("Network (station congruence) MDB TABLE (SPEC 7.3) — per station: "
              "run-median delta (cm, ENU) +- MAD, and the FORMAL coordinate MDB (m) "
              "with its unobservable-epoch count.  delta is the HELD COORDINATE'S "
              "ERROR: +East means the held mark sits that far East of where the data "
              "puts the antenna, so SUBTRACT it to correct.  'inf' is a CATEGORY (that "
              "direction is not estimable this run), never a large number to act on.");
    for (const auto& kv : acc.stations()) {
        const int sta = kv.first;
        for (int li = 0; li < kCongruenceLabels; ++li) {
            const auto lab = static_cast<CongruenceLabel>(li);
            const NetworkCongruence::Ladder& L = acc.runLadder(sta, lab);
            if (L.n_epochs == 0) continue;
            const NetworkCongruence::DeltaStats d = acc.deltaStats(sta, lab);
            const NetworkCongruence::MdbStats  q = acc.mdbStats(sta, lab);
            log->info("Network (station congruence) {}/{}: n = {} station-epoch(s) "
                      "({} rows, {} degenerate, {} null-geometry, {} raw-formal)  |  "
                      "delta = E {:+.2f}+-{:.2f}  N {:+.2f}+-{:.2f}  U {:+.2f}+-{:.2f} cm "
                      "|  MDB = E {} ({} inf)  N {} ({} inf)  U {} ({} inf) m  |  "
                      "geo(t) = {:.3g}, {} over t > {:g}",
                      stationName(stations, sta), congruenceLabelName(lab),
                      L.n_epochs, L.n_rows, L.n_degenerate, L.n_null, L.n_raw_formal,
                      100.0 * d.med(0), 100.0 * d.mad(0),
                      100.0 * d.med(1), 100.0 * d.mad(1),
                      100.0 * d.med(2), 100.0 * d.mad(2),
                      mCell(q.med(0)), q.n_inf[0], mCell(q.med(1)), q.n_inf[1],
                      mCell(q.med(2)), q.n_inf[2],
                      L.geoT(), L.n_reported, acc.reportSigma());
            // The worst-epoch identity line is written only where there is a
            // finding to name.  On a 305-station roster an unconditional second
            // line would double a 600-line table; the ladder above already carries
            // the occupancy, and invariant #4 asks for a name on a finding, not on
            // every denominator.
            if (L.n_reported > 0)
                log->info("Network (station congruence) {}/{}: worst station-epoch "
                          "t = {:.4g} (T = {:.4g}, q = {}) with delta = "
                          "({:+.4f}, {:+.4f}, {:+.4f}) m ENU @tow {:.1f}",
                          stationName(stations, sta), congruenceLabelName(lab),
                          L.max_t, L.max_T, L.max_dof, L.max_delta(0),
                          L.max_delta(1), L.max_delta(2), L.max_tow);
        }
    }

    // ---- the degenerate population, named -----------------------------------
    if (t.degenerate > 0)
        log->info("Network (station congruence): {} of {} station-epoch(s) ({:.1f} %) "
                  "had at least one UNOBSERVABLE direction — the per-epoch station "
                  "clock and ZWD absorbing vertical is the expected cause, and the "
                  "MDB table's 'inf' counts say which direction and how often.  "
                  "MEASURED absorption, not a failure.",
                  t.degenerate, t.station_sweeps,
                  100.0 * static_cast<double>(t.degenerate)
                        / static_cast<double>(std::max<long long>(1, t.station_sweeps)));
    else
        log->info("Network (station congruence): NOT ONE station-epoch was degenerate "
                  "— all three ENU directions carried information at every sweep.  On "
                  "a lineage that estimates a per-epoch station clock and a ZWD this is "
                  "worth a second look at the eigenvalue columns before it is believed.");

    // ---- the separability 2x2 (INC-3) -------------------------------------
    // A coordinate fault is coherent across all lines of sight and should not
    // excite the per-LOS block tier; a single-LOS fault should excite that tier and
    // not this one.  Mass off the diagonal is the evidence that the two tiers
    // partition the fault classes rather than duplicate each other.
    if (acc.separabilityKnown() == 0) {
        log->info("Network (station congruence): INC-3 separability is UNMEASURED — "
                  "the block-Baarda census was not armed, so no per-LOS verdict existed "
                  "to join against.  That is 'no verdict', which is NOT the same as "
                  "'the two tiers agreed'.  Arm network.block_baarda to measure it.");
    } else {
        const auto c = acc.separability();
        const long long n = c[0][0] + c[0][1] + c[1][0] + c[1][1];
        log->info("Network (station congruence): INC-3 separability over {} joined "
                  "station-epoch(s) — congruence LOUD x [block quiet {}, block flagged "
                  "{}] | congruence quiet x [{}, {}].  The a-cell (loud here, quiet "
                  "there) is the population ONLY this card can see: a coherent "
                  "all-LOS coordinate offset.  A large b-cell instead means the two "
                  "tiers are firing on the same events and this card is not adding a "
                  "fault class.",
                  n, c[0][0], c[0][1], c[1][0], c[1][1]);
    }

    // ---- no silent caps ------------------------------------------------------
    const long long dropped = acc.droppedSamples();
    if (dropped > 0)
        log->warn("Network (station congruence): {} station-epoch sample(s) were DROPPED "
                  "past the {}-per-(station x label) retention cap, so the medians and "
                  "MADs above cover the FIRST {} swept epochs of those stations, not the "
                  "whole run.  Raise the cap or shorten the run before quoting them as "
                  "run-wide numbers.",
                  dropped, kCongruenceMaxSamples, kCongruenceMaxSamples);

    if (!t.max_id.empty())
        log->info("Network (station congruence): worst station-epoch of the run "
                  "t = {:.4g} at {}.", t.max_t, t.max_id);
    log->info("Network (station congruence): MEASURE-ONLY — no coordinate was moved, "
              "deweighted or re-estimated, and no downstream consumer read any number "
              "above.  Re-coordinating a station is a DATUM act (invariant #7, both "
              "edges) and belongs to its own card.");
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the station congruence screen.
int networkCongruenceRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkSetupResult& setup = v.setup;

    // --- Station congruence run-end telemetry --------------------------------
    // (docs/SPEC_redundancy_census.md §13.7.3).  "Every held coordinate on this
    // network is right" and "the screen never swept a station" produce identical
    // silence (invariant #4), so the denominators come first.  Then comes the MDB
    // table, a service deliverable rather than a diagnostic: the smallest coordinate
    // fault this network can detect, per station and per direction.  The body is in
    // filter/NetworkCongruence.cpp, next to the sweep that produces the numbers.
    if (ctx.knobs.cg_on)
        networkCongruenceRunEnd(ctx, log, setup.stations);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

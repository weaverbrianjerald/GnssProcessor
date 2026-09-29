/**
 * @file NetworkRaim.cpp
 * @brief Implementation of @ref networkPostFitRaim, the post-fit network RAIM
 *        pass moved out of Application::runNetwork at decomposition INC-9.
 *
 * The body is the former SB_RAIM block of ApplicationNetwork.cpp, dedented 8
 * spaces, with one substitution: `plan.shared_datum` -> `shared_datum` (the run TU
 * held the shared datum inside NetworkSbasis::Plan; this seam takes only the Datum
 * it reads).  Measure-only additions made after the move are marked where they
 * appear.  The alias preamble reproduces the run-TU alias block so the moved code
 * reads the original names.  See NetworkRaim.hpp for the ordering contract.
 */

#include "filter/NetworkRaim.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.knobs / ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"              // config_ getters
#include "filter/NetworkMdb.hpp"              // Baarda MDB rider on B2's r_i (SPEC_redundancy_census)
#include "filter/NetworkRobustStats.hpp"      // medianPolish2Way / robustProminence / medianCopy / MedianPolish
#include "filter/NetworkAmbStateTrace.hpp"    // ambStateNoteErase (RF-1 §7.4 D3 erase inventory)
#include "filter/NetworkAmbFloat.hpp"          // ambFloatDrop (RF-1 step 4: the bounded drop)
#include "filter/StateManager.hpp"            // stations[i].sm->markAmbiguityPendingReseed
#include "logging/NetworkRunTraces.hpp"       // trace streams + emitEvent
#include "products/NetworkStateSnapshot.hpp"  // networkWarmupRampLifted
#include "slip/NetworkRawObservables.hpp"     // RawSlipEpoch (corroborated retap evidence)

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Band-scoped T4 erase rule (docs/SPEC_event_harness.md §Z)
// ---------------------------------------------------------------------------
RaimEraseBandScope networkRaimEraseBandScope(
    const std::vector<std::pair<std::string, double>>& sig_r, double bar) {
    RaimEraseBandScope d;
    if (sig_r.empty()) return d;
    d.act = true;
    // Smallest |r| wins the pin; ties break on the lexically lowest signal so the
    // choice is deterministic across runs, platforms and pool orderings.
    std::size_t keep = 0;
    for (std::size_t k = 1; k < sig_r.size(); ++k)
        if (sig_r[k].second < sig_r[keep].second
            || (sig_r[k].second == sig_r[keep].second
                && sig_r[k].first < sig_r[keep].first))
            keep = k;
    d.keep = sig_r[keep].first;
    if (sig_r.size() == 1) { d.single_band = true; return d; }
    for (std::size_t k = 0; k < sig_r.size(); ++k)
        if (k != keep && sig_r[k].second > bar) d.erase.insert(sig_r[k].first);
    d.kept_over_gate = sig_r[keep].second > bar;
    // No band cleared the gate: the cell was flagged by the median-polish
    // interaction (net of the row and column effects), so a per-signal residual
    // can sit under k·σ̂₀ while the arc still stands out.  Erase every band except
    // the pin; missing per-band evidence does not suppress the response.
    if (d.erase.empty())
        for (std::size_t k = 0; k < sig_r.size(); ++k)
            if (k != keep) d.erase.insert(sig_r[k].first);
    return d;
}

void networkPostFitRaim(NetworkRunContext& ctx,
                        const GnssConfig& config_,
                        const std::shared_ptr<spdlog::logger>& log,
                        NetworkRunTraces& traces,
                        const SyncedEpoch& se,
                        std::vector<NetStation>& stations,
                        const std::vector<StationAssembly>& asm_res,
                        const std::vector<int>& est_index_of,
                        const std::vector<bool>& present,
                        const Eigen::VectorXd& x_s,
                        const NetworkEstimator& est,
                        const NetworkEstimator::Datum& shared_datum,
                        std::set<std::string>& clock_refloat_sats,
                        std::set<std::string>& retro_flag_sats,
                        bool retro_feed,
                        int& n_arc_refloat_epoch,
                        NetworkPriorResidual& prires,
                        const RawSlipEpoch* slip_ep) {
    // --- Alias preamble: byte-copies of the former Application::runNetwork alias
    //     block, so the relocated SB_RAIM body below reads the original names. ---
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const double raim_k        = ctx.knobs.raim_k;      // standardized post-fit residual flag threshold
    const double raim_s0_floor = ctx.knobs.raim_s0_floor; // floor on the robust a-posteriori s0 (1.0 default; lower lets s0 < 1)
    const int    raim_min_sta  = ctx.knobs.raim_min_sta; // cross-station consensus count
    const int    raim_arc_persist = ctx.knobs.raim_arc_persist; // consecutive-epoch persistence gate for isolated-arc refloats (1 = act immediately, byte-identical)
    const bool   raim_maturity_gate   = ctx.knobs.raim_maturity_gate;   // gate isolated-arc refloats on maturity (T2d)
    const int    raim_maturity_epochs = ctx.knobs.raim_maturity_epochs;  // maturity horizon (T2d)
    const bool   raim_redun    = ctx.knobs.raim_redun; // Baarda per-row r_i normalization of the RAIM residual (B2)
    const bool   raim_redun_trace = ctx.knobs.raim_redun_trace; // compute r_i for the census without applying it (§7.1)
    // MDB rider on the B2 r_i (docs/SPEC_redundancy_census.md, MDB-INC-0): a
    // measure-only addition made after the move, reading the (w, r_i) pairs the
    // Pass-1 row walk already visits.  mdb_on is the master switch and
    // mdb_from_raim selects this seam as the feed site; both must be set.
    const bool   mdb_on        = ctx.knobs.mdb_on && ctx.knobs.mdb_from_raim;
    const double mdb_delta0    = baardaDelta0(config_.dataSnoopingCritical());
    // Data side of the constraint-aware global variance component
    // (docs/SPEC_prior_residual_census.md §14.2).  Also a measure-only addition made
    // after the move: it reads the same (w, v, r_i) triples the Pass-1 walk visits,
    // and nothing in this seam reads its result back.  globalOn() already requires
    // a live census, so one bool suffices here.
    const bool   s0g_on        = prires.globalOn();
    const bool   clk_mon_couple = ctx.knobs.clk_mon_couple;  // also refloat RAIM-flagged sats' clocks (opt-in)
    const bool   ura_on        = ctx.knobs.ura_on;                  // emit the per-sat URA quality channel (B1)
    const double ura_alpha     = ctx.knobs.ura_alpha;             // s0 EWMA smoothing factor (B1)
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   trace_flag_rates = ctx.knobs.trace_flag_rates;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;

    // The walks below read each station's ambiguity columns through ambPool().
    // Under carry_invariant the phase-leveling list R.pl_amb is never filled, so
    // with network.raim_carry_invariant on, assembly builds R.qc_amb instead: every
    // ambiguity column present this epoch and not gauge-pinned, whichever carry
    // owns it.  (The continuous phase-datum handover also fills R.qc_amb.)  With
    // both off, R.qc_amb is empty and ambPool() returns R.pl_amb, so those runs are
    // unchanged.  Design: docs/SPEC_raim_pool_rehost.md.
    const bool raim_ci_on      = ctx.knobs.raim_ci_on;
    const bool raim_ci_refloat = ctx.knobs.raim_ci_refloat;
    auto ambPool = [&](const StationAssembly& R) -> const std::vector<PlAmb>& {
        return R.qc_amb.empty() ? R.pl_amb : R.qc_amb;
    };

    auto& local_carry = ctx.carry.local_carry;
    // Elevation power of the code weights: the mapping in which both ambiguity
    // variance floors are stated (ambFloatBirthFloor).  Read once per pass (RF-3a A1).
    const double amb_elev_power_ = config_.getCodeElevationPower();
    // The drop floor in force this epoch (0 while the warm-up gate is closed), and a
    // flag set when the gate is what withholds it.  Both are resolved once per pass,
    // next to the elevation power, so the T4 and T3 sites below and the free-epoch
    // latch all read the same value (RF-3c).
    const double amb_drop_floor_now_ = ambFloatDropFloorNow(
        ctx.knobs.amb_drop_var_floor,
        static_cast<long long>(ctx.counters.n_loop_epochs),
        ctx.knobs.amb_drop_floor_after_ep);
    const bool   amb_drop_pre_gate_ =
        (ctx.knobs.amb_drop_var_floor > 0.0) && !(amb_drop_floor_now_ > 0.0);
    auto& raim_refloat_pending = ctx.carry.raim_refloat_pending;
    // Riders R2 and R4 (staf_on, rate_hygiene) are measure-only; the loader requires
    // network.raim for both (SPEC_t3_null_derivation §6, queue item 4).  R2 also
    // needs per-row r_i, taken from the same solve-neutral rowRedundancy read the
    // census uses (hence staf_on in the disjunct below).
    const bool staf_on      = ctx.knobs.staf_on;
    const bool rate_hygiene = ctx.knobs.rate_hygiene;
    auto&      staf_out     = traces.station_factor_trace;
    auto& raim_ci_tallies = ctx.counters.raim_ci;
    // Denominator #1 (standing invariant #4): "the seam never ran" must be
    // distinguishable from "it ran and screened nothing".
    if (raim_ci_on) ++raim_ci_tallies.epochs;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& arc_spike_streak = ctx.carry.arc_spike_streak;
    auto& ura_ewma_updates = ctx.carry.ura_ewma_updates;

    auto& n_emitted = ctx.counters.n_emitted;
    auto& fr_raim_cell = ctx.counters.fr_raim_cell;
    auto& fr_raim_sat = ctx.counters.fr_raim_sat;
    auto& fr_attrib = ctx.counters.fr_attrib;
    auto& fr_attrib_arc = ctx.counters.fr_attrib_arc;

    auto& raim_trace           = traces.raim_trace;
    auto& raim_attrib_trace    = traces.raim_attrib_trace;
    auto& events_trace         = traces.events_trace;
    auto& station_res_trace    = traces.station_res_trace;
    auto& raim_redun_out       = traces.raim_redun_trace;   // per-row r_i census (§7.1)

    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };

    // Pass 1: reconstruct each station once, form its post-fit phase residuals, and
    // record per (station, satellite) the largest v·√w (measurement-standardized),
    // pooling every phase-row value for the robust a-posteriori variance factor.
    struct AmbStat { std::string sat; int sta; double r_max; };
    std::vector<AmbStat> amb_stats;
    std::vector<double>  r_pool;
    // Prominence-attribution input (collected iff attribution is logged or drives the
    // action): one cell per (station, satellite, signal) carried-ambiguity residual.
    const bool attrib_on   = raim_attrib_trace.is_open();       // log the decomposition
    const bool attrib_act  = config_.networkRaimAttribution();  // let it drive the refloat
    // The attribution table is also built under trace_flag_rates, so the T4 flag rate
    // can be measured on a baseline with network.trace_flag_rates as the only armed
    // knob (the documented empirical-α workflow), without raim_attribution or the
    // attribution trace.  The median polish then runs read-only (no refloat unless
    // attrib_act, no trace unless attrib_on) and feeds only the fr_attrib counter.
    // With trace_flag_rates off the output is byte-identical (B8/T4, P1-4).
    const bool need_attrib = attrib_on || attrib_act || trace_flag_rates;
    struct Cell { int sta; std::string sat, sig; double r; };
    std::vector<Cell> cells;
    // Per-station factor rows (R2), collected during Pass 1 and reduced once σ̂₀
    // exists (the trim bar is k·σ̂₀, which is not known until the pool closes).
    std::vector<std::vector<StationFactorRow>> staf_rows;
    if (staf_on) staf_rows.resize(stations.size());
    for (size_t i = 0; i < stations.size(); ++i) {
        // The pivot station is skipped only when network.phase_datum_prioritized is
        // off.  With it on, the pivot is screened: its non-anchored arcs are in
        // R.pl_amb (anchored and pinned keys are excluded when the carry is built).
        if (est_index_of[i] < 0
            || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
        const StationAssembly& R = asm_res[i];
        const std::vector<PlAmb>& pool = ambPool(R);   // pool re-host: qc_amb or pl_amb
        if (pool.empty() || R.A_s.cols() != x_s.size()) continue;
        if (raim_ci_on) {
            ++raim_ci_tallies.pool_stations;
            raim_ci_tallies.pool_cells += static_cast<long long>(pool.size());
        }
        Eigen::VectorXd x_r;
        try {
            x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
        } catch (...) { continue; }
        if (x_r.size() != R.A_r.cols()) continue;
        const Eigen::VectorXd resid = R.y - R.A_s * x_s - R.A_r * x_r;  // post-fit O − C
        Eigen::VectorXd row_r;   // per-row Baarda redundancy (B2; empty when off)
        // The census also needs r_i, without the standardization consuming it
        // (SPEC_t3_null_derivation §7.1).  Arming raim_redundancy_normalize is
        // solve-affecting (it drops rows at the floor), so a diagnostic that required it
        // could not run on a validated posture.  Computing row_r is a pure read of the
        // already-factored normal equations, so the disjunct below is solve-neutral: the
        // value is applied only where `raim_redun` itself is tested.
        if (raim_redun || raim_redun_trace || staf_on) {
            try {
                row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                          R.A_s, R.A_r, R.w, shared_datum);
            } catch (...) { row_r.resize(0); }
        }
        // The station factor (R2) needs a full r_i vector.  A rowRedundancy throw
        // leaves the station unavailable this epoch (the trace writes the −1
        // sentinel, never 0).
        const bool staf_here = staf_on && row_r.size() == R.A_r.rows();
        const bool redun_trace_here = raim_redun_out.is_open();
        // --- MDB rider (docs/SPEC_redundancy_census.md MDB-INC-0) ------------
        // Every row this loop visits already has (w, r_i) in hand, so its minimal
        // detectable bias costs one division.  The loop below is keyed on ambiguity
        // columns, not rows, so a row touched by two carried ambiguities is visited
        // twice; the visited mask bins each row once, keeping the join against the
        // census free of duplicates.
        const bool mdb_here = mdb_on && raim_redun && row_r.size() == R.A_r.rows();
        std::vector<char> mdb_seen;
        std::optional<NetworkMdbStationFeed> mdb_feed;
        if (mdb_here) {
            mdb_seen.assign(static_cast<std::size_t>(R.A_r.rows()), 0);
            mdb_feed.emplace(ctx.carry.mdb, se.time.tow, mdb_delta0,
                             ctx.knobs.mdb_event_m, stations[i].name,
                             NetMdbSource::Raim, 0.0);
        }
        // --- constraint-aware global σ̂₀, data side (SPEC_prior_residual_census §14.2) --
        // This rider has its own visited mask, separate from the MDB one: MDB
        // requires `raim_redun`, this rider does not (without it, it uses r ≡ 1 and
        // flags the epoch), so the two row populations differ, and a shared mask
        // would give whichever rider ran second only a subset.  The mask is needed
        // for the same reason as MDB's: the loop is keyed on ambiguity columns, so a
        // row touched by two carried ambiguities would be counted twice, inflating
        // both sums while leaving their ratio plausible.
        const bool s0g_redun_here = raim_redun && row_r.size() == R.A_r.rows();
        std::vector<char> s0g_seen;
        if (s0g_on) s0g_seen.assign(static_cast<std::size_t>(R.A_r.rows()), 0);
        std::vector<char> staf_seen;   // row de-dup for R2 (column-keyed loop, as for MDB)
        if (staf_here) staf_seen.assign(static_cast<std::size_t>(R.A_r.rows()), 0);
        // Largest measurement-standardized post-fit phase residual per satellite at this
        // station (a carried ambiguity's phase rows carry +1 on its A_r column; uncombined).
        std::map<std::string, double> sat_rmax;
        // ZWD in the carried block (network.carry_zwd_in_block, B9-in-block;
        // SPEC_amb_iono_split_overcount §7 item 4).  The pool admits a row by
        // |A_r(row, amb_col)| > 0.5, a test written for data phase rows (which carry
        // exactly +1 on their ambiguity column).  A whitened block row is one row of L⁻¹,
        // so the ZWD's block row carries non-zero coefficients on the ambiguity columns
        // below it and can clear that bar: a carried-prior row can enter a post-fit fault
        // pool built for observations.  The admission rule is unchanged here; the
        // admission is counted so the rule can be decided on data.  The ZWD's row is the
        // coupling row whose primary local column is the station's ZWD column
        // (cpl_row_local_cols is in append order from cpl_row_start).
        Eigen::Index zwd_blk_row = -1;
        if (R.pl_zwd_in_block && R.cpl_row_start >= 0 && R.pl_zwd.col >= 0)
            for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                if (R.cpl_row_local_cols[rr] == R.pl_zwd.col) {
                    zwd_blk_row = R.cpl_row_start + static_cast<Eigen::Index>(rr);
                    break;
                }
        bool zwd_row_pooled = false;   // once per station-epoch (the column-keyed loop can revisit it)
        // Iono in the carried block (network.carry_iono_in_block, I-in-block): the same
        // §7 item 4 interaction for the I half.  Each carried iono column contributes one
        // whitened block row (a row of L⁻¹), which carries non-zero coefficients on the
        // ambiguity columns below it and can therefore clear the pool's
        // |A_r(row, amb_col)| > 0.5 admission test written for data phase rows.  The rule
        // is unchanged here; the admissions are counted so it can be decided on data.  The
        // rows are the coupling rows whose primary local column is one of pl_iono_blk_cols
        // (cpl_row_local_cols is in append order from cpl_row_start).
        std::set<Eigen::Index> iono_blk_rows;
        if (!R.pl_iono_blk_cols.empty() && R.cpl_row_start >= 0) {
            const std::set<Eigen::Index> icols(R.pl_iono_blk_cols.begin(),
                                               R.pl_iono_blk_cols.end());
            for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                if (icols.count(R.cpl_row_local_cols[rr]))
                    iono_blk_rows.insert(R.cpl_row_start + static_cast<Eigen::Index>(rr));
        }
        std::set<Eigen::Index> iono_rows_pooled;   // once per row per station-epoch
        // Receiver clock in the carried block (network.carry_rx_clock_in_block,
        // RX-CLK-in-block): the same §7 item 4 interaction for the rho half.  Each carried
        // clock-family column contributes one whitened block row (a row of L⁻¹) carrying
        // non-zero coefficients on the ambiguity columns below it, which can therefore
        // clear the pool's |A_r(row, amb_col)| > 0.5 admission test written for data
        // phase rows.  This count is expected to be large wherever the ZWD's is: the
        // receiver clock is the most station-common direction, so its whitened row touches
        // every ambiguity column of the station.  The rule is unchanged here; the
        // admissions are counted so it can be decided on data.
        std::set<Eigen::Index> rxclk_blk_rows;
        if (!R.pl_rxclk_blk_cols.empty() && R.cpl_row_start >= 0) {
            const std::set<Eigen::Index> rcols(R.pl_rxclk_blk_cols.begin(),
                                               R.pl_rxclk_blk_cols.end());
            for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                if (rcols.count(R.cpl_row_local_cols[rr]))
                    rxclk_blk_rows.insert(R.cpl_row_start + static_cast<Eigen::Index>(rr));
        }
        std::set<Eigen::Index> rxclk_rows_pooled;   // once per row per station-epoch
        for (const auto& pa : pool) {
            if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
            double r_max = 0.0;
            for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
                if (row == zwd_blk_row && !zwd_row_pooled) {
                    zwd_row_pooled = true;
                    ++ctx.counters.n_raim_pool_zwd_rows;
                }
                // Same count for iono block rows (I-in-block).
                if (!iono_blk_rows.empty() && iono_blk_rows.count(row)
                    && iono_rows_pooled.insert(row).second)
                    ++ctx.counters.n_raim_pool_iono_rows;
                // Same count for clock-family block rows (RX-CLK-in-block).
                if (!rxclk_blk_rows.empty() && rxclk_blk_rows.count(row)
                    && rxclk_rows_pooled.insert(row).second)
                    ++ctx.counters.n_raim_pool_rxclk_rows;
                const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                // MDB is fed here, ahead of the `continue` below that skips uncheckable
                // rows: a row RAIM drops from its σ̂0 pool because r_i ≤ 1e-6 is a row
                // whose MDB is +infinity, and that value is what the rider publishes.
                // Binning only the rows that survive the drop would report the network
                // as fully checkable by construction.
                if (mdb_here && !mdb_seen[static_cast<std::size_t>(row)]) {
                    mdb_seen[static_cast<std::size_t>(row)] = 1;
                    mdb_feed->add(pa.sat, NetRowClass::ObsPhase, w, row_r(row));
                }
                // The global variance component is also fed ahead of that `continue`.
                // Since E[w·v²] = r_i, an r_i ≈ 0 row contributes ~0 to both the
                // numerator and the denominator and cannot bias the ratio, whereas
                // dropping it would remove a real row from Σr_data and report the
                // pool as more redundant than it is.
                if (s0g_on && !s0g_seen[static_cast<std::size_t>(row)]) {
                    s0g_seen[static_cast<std::size_t>(row)] = 1;
                    prires.addDataRow(w, resid(row),
                                      s0g_redun_here ? row_r(row) : 1.0,
                                      /*r_is_approx=*/!s0g_redun_here);
                }
                double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                // The R2 station factor is fed ahead of the same `continue`, for the
                // same reason as the global component (E[w·v²] = r_i: an r_i ≈ 0 row
                // contributes ~0 to both sums, while dropping it would overstate the
                // station's redundancy).  It has its own seen-mask because the loop is
                // keyed on ambiguity columns, so a row touched by two carried
                // ambiguities would otherwise inflate both sums (as for MDB and s0g).
                // The trim statistic is always |v|·√w; the B2 division is a pool
                // convention, not part of the §7.5 factor.
                if (staf_here && !staf_seen[static_cast<std::size_t>(row)]) {
                    staf_seen[static_cast<std::size_t>(row)] = 1;
                    staf_rows[i].push_back(
                        {w * resid(row) * resid(row), row_r(row), r});
                }
                // Redundancy census (§7.1), also written ahead of the `continue` below,
                // as for the MDB and global σ̂₀ feeds: the rows the normalization drops
                // are the ones the census exists to show, and a census of survivors
                // could not show a floor problem by construction.  One row per (sta,
                // sat, signal, row); the per-cell max discards the signal.
                if (redun_trace_here) {
                    const bool has_r = row < row_r.size();
                    const double ri  = has_r ? row_r(row) : 0.0;
                    raim_redun_out
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << stations[i].name << ',' << pa.sat << ',' << pa.signal << ','
                        << row << ',' << fmt::format("{:.6e}", w) << ',';
                    if (has_r) raim_redun_out << fmt::format("{:.6e}", ri);
                    raim_redun_out
                        << ',' << fmt::format("{:.6e}", resid(row))
                        << ',' << fmt::format("{:.6e}", r) << ',';
                    if (has_r && ri > 0.0)
                        raim_redun_out << fmt::format("{:.6e}", r / std::sqrt(ri));
                    raim_redun_out
                        << ',' << ((has_r && ri <= kBaardaRedundancyFloor) ? 1 : 0) << '\n';
                }
                if (raim_redun && row < row_r.size()) {   // standardize v/(σ·√r_i) (B2)
                    constexpr double kRedFloor = kBaardaRedundancyFloor;
                    const double ri = row_r(row);
                    if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                    r /= std::sqrt(ri);
                }
                r_pool.push_back(r);
                if (r > r_max) r_max = r;
            }
            double& cur = sat_rmax[pa.sat];
            if (r_max > cur) cur = r_max;
            if (need_attrib) cells.push_back({static_cast<int>(i), pa.sat, pa.signal, r_max});
        }
        if (mdb_here) mdb_feed->finish();   // commit this station's event candidates
        for (const auto& [sat, rmax] : sat_rmax)
            amb_stats.push_back({sat, static_cast<int>(i), rmax});
    }

    // Robust a-posteriori variance factor for this epoch, network-wide (1.4826 × the
    // median of the pool).  Floored at raim_s0_floor (1.0 by default) so a clean network
    // is no more sensitive than the nominal stochastic model (σ̂0 = 1 gives the plain
    // standardized residual).  When the whole network misfits at cm level, σ̂0 rises and
    // the normalized residuals stay O(1), so RAIM flags only what stands out above the
    // ambient misfit.
    double sigma0 = 1.0;
    if (!r_pool.empty()) {
        std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
        sigma0 = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
    }
    // Pool re-host telemetry.  With an empty pool, r_pool is empty and sigma0 keeps its
    // 1.0 initializer; σ̂0 is recorded only over epochs with a non-empty pool, so that
    // sentinel does not dilute the mean.
    if (raim_ci_on && !r_pool.empty()) {
        raim_ci_tallies.sigma0_max = std::max(raim_ci_tallies.sigma0_max, sigma0);
        raim_ci_tallies.sigma0_sum += sigma0;
        ++raim_ci_tallies.sigma0_n;
    }
    // Fold the network-wide σ̂0 into the URA EWMA (B1).  This seam runs after emission,
    // so the new scale applies to the URA emitted next epoch.  The update count feeds
    // the quality-ramp lift rule (its "EWMA has stabilized" half) and is read only
    // while the warm-up floor is armed (RTB-7(b)).
    if (ura_on) {
        ura_sigma0_ewma = ura_alpha * sigma0 + (1.0 - ura_alpha) * ura_sigma0_ewma;
        ++ura_ewma_updates;
    }

    // --- Declared per-station variance factor (R2, queue item 4) ---------------
    // (SPEC_t3_null_derivation §6/§7.5.)  Measure-only: nothing below feeds any
    // denominator, gate or response.  Absorbing the factor is R1, which is held
    // because it would bury the measured per-station iono error (§7.8.2).  Runs
    // after σ̂₀ because the trim bar is k·σ̂₀.  The seam runs only on accepted
    // epochs, so the EWMA is frozen across withholds.
    if (staf_on) {
        const double now_tow = se.time.tow;
        double dt = (ctx.carry.staf_prev_tow >= 0.0)
                        ? std::max(0.0, now_tow - ctx.carry.staf_prev_tow) : 0.0;
        const double alpha =
            (dt > 0.0) ? std::min(1.0, dt / std::max(1.0, ctx.knobs.staf_tau_s)) : 1.0;
        struct StafOut { int sta; double f, ewma; std::size_t n, n_trim; };
        std::vector<StafOut> outs;
        const double trim_above = raim_k * sigma0;
        for (size_t i = 0; i < staf_rows.size(); ++i) {
            if (staf_rows[i].empty()) continue;
            const double f = stationVarianceFactor(staf_rows[i], trim_above);
            std::size_t n_trim = 0;
            for (const auto& q : staf_rows[i]) if (q.r_std > trim_above) ++n_trim;
            double ewma = f;
            if (f >= 0.0) {
                auto it = ctx.carry.staf_ewma.find(static_cast<int>(i));
                if (it == ctx.carry.staf_ewma.end()) {
                    ctx.carry.staf_ewma[static_cast<int>(i)] = f;   // seed
                } else {
                    it->second += alpha * (f - it->second);
                    ewma = it->second;
                }
                ++ctx.carry.staf_updates[static_cast<int>(i)];
            }
            outs.push_back({static_cast<int>(i), f, ewma,
                            staf_rows[i].size(), n_trim});
        }
        if (!outs.empty()) {
            ++ctx.counters.n_staf_epochs;
            // The depart statistic: EWMA ratio to the network median of EWMAs.
            std::vector<double> ew;
            ew.reserve(ctx.carry.staf_ewma.size());
            for (const auto& kv : ctx.carry.staf_ewma) ew.push_back(kv.second);
            const double med = std::max(1e-12, medianCopy(ew));
            for (const auto& o : outs) {
                const double ratio = ctx.carry.staf_ewma.count(o.sta)
                                         ? ctx.carry.staf_ewma.at(o.sta) / med : 0.0;
                if (ratio > ctx.counters.staf_max_ratio)
                    ctx.counters.staf_max_ratio = ratio;
                const bool departed = ctx.carry.staf_departed.count(o.sta) > 0;
                // Declaration warm-up: no edge is declared before the EWMA has 10
                // updates (about 5 min at 30 s), so a new station does not trigger
                // off its seed value.
                const bool warm = ctx.carry.staf_updates[o.sta] >= 10;
                if (warm && !departed && ratio > ctx.knobs.staf_ratio) {
                    ctx.carry.staf_departed.insert(o.sta);
                    ++ctx.counters.n_staf_departs;
                    if (events_trace.is_open())
                        emitEvent("T3_stafactor", "depart", "", stations[o.sta].name,
                                  ratio, ctx.knobs.staf_ratio, "declare",
                                  fmt::format("ewma={:.3f};net_med={:.3f}",
                                              ctx.carry.staf_ewma.at(o.sta), med));
                } else if (departed && ratio < 0.8 * ctx.knobs.staf_ratio) {
                    ctx.carry.staf_departed.erase(o.sta);
                    ++ctx.counters.n_staf_returns;
                    if (events_trace.is_open())
                        emitEvent("T3_stafactor", "return", "", stations[o.sta].name,
                                  ratio, 0.8 * ctx.knobs.staf_ratio, "declare",
                                  fmt::format("ewma={:.3f};net_med={:.3f}",
                                              ctx.carry.staf_ewma.at(o.sta), med));
                }
                if (staf_out.is_open())
                    staf_out << se.time.week << ','
                             << fmt::format("{:.3f}", now_tow) << ','
                             << stations[o.sta].name << ','
                             << fmt::format("{:.4f}", o.f) << ','
                             << fmt::format("{:.4f}", o.ewma) << ','
                             << fmt::format("{:.4f}", ratio) << ','
                             << o.n << ',' << o.n_trim << ','
                             << (ctx.carry.staf_departed.count(o.sta) ? 1 : 0)
                             << '\n';
            }
        }
        ctx.carry.staf_prev_tow = now_tow;
    }

    // Pass 2: normalize by σ̂0 and take the cross-station consensus per satellite.
    std::map<std::string, int> sat_flag_stations;  // satellite -> #stations whose normalized residual spiked
    struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bool spiked; };
    std::vector<RaimDetail> raim_details;           // per (station, sat); built only when tracing
    std::map<std::string, int> sat_cell_stations;  // per-sat cell count for R4 (unconditional)
    for (const auto& a : amb_stats) {
        const double s_norm = a.r_max / sigma0;
        const bool   spiked = (s_norm > raim_k);
        if (rate_hygiene) ++sat_cell_stations[a.sat];
        // Cell flag rate for tier T3 (B8/T3).  Under R4, a cell with s_raw exactly 0 (a carried
        // ambiguity with no phase row this epoch) is untestable: it can never flag,
        // and counting it as an opportunity deflates the reported rate (§4).
        if (trace_flag_rates) {
            if (rate_hygiene && a.r_max == 0.0) ++ctx.counters.n_t3_untestable_cells;
            else ++fr_raim_cell.opp;
            if (spiked) ++fr_raim_cell.flag;
        }
        if (station_res_trace.is_open())   // L13: every cell (not just spikes)
            station_res_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                              << ',' << stations[a.sta].name << ',' << a.sat << ','
                              << fmt::format("{:.3f}", a.r_max) << ','
                              << fmt::format("{:.3f}", s_norm) << ','
                              << fmt::format("{:.3f}", sigma0) << '\n';
        if (spiked) ++sat_flag_stations[a.sat];
        if (raim_trace.is_open())
            raim_details.push_back({a.sat, a.sta, a.r_max, s_norm, spiked});
    }
    std::vector<std::string> flagged;
    for (const auto& kv : sat_flag_stations)
        if (kv.second >= raim_min_sta) flagged.push_back(kv.first);
    const std::set<std::string> fset(flagged.begin(), flagged.end());
    if (raim_ci_on) raim_ci_tallies.sats_flagged += static_cast<long long>(flagged.size());
    if (trace_flag_rates) {   // B8/T3 consensus: rate of consensus among sats that spiked ≥1 station
        // Without R4 hygiene the opportunity count is conditional (only satellites that
        // spiked at some station), so implied_alpha compares a conditional rate with an
        // unconditional α (§1).  Under hygiene the opportunity is the unconditional
        // population: every satellite with cells at ≥ raim_min_sta stations this epoch
        // (the satellites structurally able to reach consensus).
        if (rate_hygiene) {
            long uncond = 0;
            for (const auto& kv : sat_cell_stations)
                if (kv.second >= raim_min_sta) ++uncond;
            fr_raim_sat.opp += uncond;
        } else {
            fr_raim_sat.opp += static_cast<long>(sat_flag_stations.size());
        }
        fr_raim_sat.flag += static_cast<long>(flagged.size());
    }

    // A consensus-flagged satellite's slip has leaked into its shared clock, which is why
    // stations without the slip spiked too.  With clk_mon_couple set, the carried clock is
    // refloated along with the ambiguity.
    if (clk_mon_couple)
        for (const auto& s : fset) clock_refloat_sats.insert(s);
    // Consensus-flagged sats have contaminated the emitted correction, so they feed the
    // retro-bump whether or not clk_mon_couple is set (P0-2).
    if (retro_feed)
        for (const auto& s : fset) retro_flag_sats.insert(s);

    // Trace the consensus context: every station of a refloated sat (to see the
    // guilty-hidden / innocent-spike pattern) plus any spiked near-miss.  Quiet
    // epochs emit nothing.  σ̂0 is logged so the all-move-together epochs (where σ̂0
    // itself spikes) are diagnosable.
    if (raim_trace.is_open()) {
        for (const auto& d : raim_details) {
            if (!(fset.count(d.sat) || d.spiked)) continue;
            raim_trace
                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                << d.sat << ',' << stations[d.sta].name << ','
                << fmt::format("{:.3f}", d.s_raw) << ',' << fmt::format("{:.3f}", d.s_norm) << ','
                << fmt::format("{:.3f}", sigma0) << ',' << fmt::format("{:.1f}", raim_k) << ','
                << (d.spiked ? 1 : 0) << ',' << sat_flag_stations[d.sat] << ','
                << raim_min_sta << ',' << (fset.count(d.sat) ? 1 : 0) << '\n';
        }
    }

    // --- Prominence attribution + optional attribution-driven refloat -------------
    // A robust 2-way median polish of the (station × satellite) standardized-residual
    // table separates a bad satellite (whole column raised), a bad station (whole row)
    // and an isolated arc (interaction); a per-signal marginal on the polished leftover
    // flags a bad signal.  It is one pass over the residual cells already in hand, with
    // no re-solve (unlike the user-side iterative snoop).  Always logged when the
    // attribution trace is open; with network.raim_attribution on it also drives the
    // refloat (bad sat → network-wide, bad station → that station's arcs, isolated
    // arc → that (station,sat) only) in place of the per-satellite consensus.
    bool attrib_acted = false;
    if (need_attrib && !cells.empty()) {
        constexpr double kAttribReportProm = 4.0;  // robust-z above which an entity is logged

        // (station × sat) table: cell = max standardized residual over that
        // station-sat's signals.
        std::map<std::pair<int, std::string>, double> cell_max;
        for (const auto& c : cells) {
            const auto key = std::make_pair(c.sta, c.sat);
            auto it = cell_max.find(key);
            if (it == cell_max.end() || c.r > it->second) cell_max[key] = c.r;
        }
        std::vector<std::tuple<int, std::string, double>> tbl;
        tbl.reserve(cell_max.size());
        for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
        const MedianPolish mp = medianPolish2Way(tbl);

        // Attribution flag rate on the per-satellite median-polish column effect
        // (B8/T4, P1-4), with the criterion the acting path (bad_sats) uses: σ̂0 above
        // the within-epoch trigger and effect/σ̂0 above raim_k.  The rate is therefore
        // measurable on a clean baseline whether or not raim_attribution acts.
        if (trace_flag_rates) {
            const double trig_a = config_.networkRaimSigma0Trigger();
            for (const auto& [sat, eff] : mp.col_eff) {
                (void)sat;
                ++fr_attrib.opp;
                if (sigma0 > trig_a && sigma0 > 0.0 && eff / sigma0 > raim_k) ++fr_attrib.flag;
            }
            // Criterion rate of the arc sub-path (§9.7.10).  The satellite line above
            // requires the σ̂0 trigger, so it never flags on a clean epoch, while the
            // arc sub-path (the one that refloats isolated slips) fires at any σ̂0.
            // Without this line the summary would read "T4: N opp, 0 flag" on runs
            // where T4 refloated arcs (the §0 error class: an instrument that does not
            // record the acting sub-path).  Criterion only, before the persistence and
            // maturity gates, as the satellite line records the bad_sats criterion.
            for (std::size_t k = 0; k < mp.interaction.size(); ++k) {
                ++fr_attrib_arc.opp;
                if (sigma0 > 0.0 && mp.interaction[k] / sigma0 > raim_k)
                    ++fr_attrib_arc.flag;
            }
        }

        const auto sat_prom = robustProminence(mp.col_eff);
        const auto sta_prom = robustProminence(mp.row_eff);

        // Signal marginal on the polished leftover (net of overall + station + sat).
        std::map<std::string, std::vector<double>> sig_left;
        for (const auto& c : cells)
            sig_left[c.sig].push_back(
                c.r - mp.overall - mp.row_eff.at(c.sta) - mp.col_eff.at(c.sat));
        std::map<std::string, double> sig_eff;
        for (auto& [sig, v] : sig_left) sig_eff[sig] = medianCopy(v);
        const auto sig_prom = robustProminence(sig_eff);

        // Isolated-arc interaction scale.
        std::vector<double> inter_abs;
        inter_abs.reserve(mp.interaction.size());
        for (double v : mp.interaction) inter_abs.push_back(std::abs(v));
        const double inter_mad = std::max(1e-9, 1.4826 * medianCopy(inter_abs));

        // Attribution-driven refloat set (scale-consistent: an effect is "bad" when it
        // stands raim_k robust baselines (σ̂0) above the additive model).  A bad
        // satellite/station is caught wholesale; an isolated arc counts only if its
        // sat/station were not already caught (no double attribution).
        std::set<std::string> bad_sats;
        std::set<int>         bad_stations;   // deliberately never populated; see below
        std::vector<std::pair<int, std::string>> bad_arcs;
        if (attrib_act) {
            // Wholesale satellite refloat happens only when the network as a whole is
            // contaminated (σ̂0 above the within-epoch trigger).  At a robust-clean epoch
            // (σ̂0 ≈ 1) a high column effect is a median-polish artifact of a satellite
            // visible across a regional station cluster, not a network-wide slip;
            // refloating its carriers everywhere discards accumulated precision and
            // destabilizes the solution (a multi-hour lock-in at 305 stations).  The
            // isolated-arc path below still refloats real single slips at any σ̂0.
            if (sigma0 > config_.networkRaimSigma0Trigger())
                for (const auto& [sat, eff] : mp.col_eff)
                    if (eff / sigma0 > raim_k) bad_sats.insert(sat);
            // With clk_mon_couple set, the carried clock of an attribution-flagged
            // (network-wide) satellite is refloated too.  An isolated arc is a local slip
            // and does not implicate the shared clock, so only whole-satellite culprits
            // feed the clock refloat.
            if (clk_mon_couple)
                for (const auto& s : bad_sats) clock_refloat_sats.insert(s);
            // Whole-satellite attribution culprits have contaminated the emitted
            // correction, so they feed the retro-bump whether or not clk_mon_couple is
            // set (P0-2).  Isolated arcs are local slips that do not implicate the shared
            // per-sat correction, so they are excluded, as in the clock-refloat scoping.
            if (retro_feed)
                for (const auto& s : bad_sats) retro_flag_sats.insert(s);
            // Bad-station wholesale refloat is disabled on purpose; bad_stations stays
            // empty.  A regional cluster of stations raised by one shared satellite event
            // inflates their median-polish row effects, so this path refloated dozens of
            // unaffected stations at once, and the cascade diverged the solution.  A bad
            // station is handled by the within-epoch station exclude (σ̂0 > trigger) or
            // surfaces as isolated arcs; a whole-station carrier wipe is not worth the
            // mis-attribution risk.
            // Persistence gate (network.raim_arc_persist_epochs, K): an isolated arc is
            // refloated only after it trips the interaction gate on K consecutive epochs.  A
            // real slip persists as an integer step until refloated, so it trips every epoch,
            // reaches K and is refloated; a heavy-tail or model-error spike does not repeat on
            // the same arc and never reaches K.  K = 1 (default) takes the immediate path
            // below and is byte-identical.  For K > 1 the streak map is rebuilt from this
            // epoch's flagged set, so any arc not flagged now resets to 0 (absence means
            // non-consecutive), and an arc that reaches K is refloated and dropped from the
            // map (it must persist another K epochs before its next refloat, so a persistent
            // arc refloats at most once per K epochs).
            // Filter-maturity latch for the isolated-arc refloat gate (T2d).  It mirrors the
            // warm-up URA ramp: an emitted-epoch horizon and a σ̂0-EWMA update count, the
            // count waived when the EWMA is structurally unavailable.  Isolated arcs drive a
            // self-sustaining warm-up churn, so they are suppressed until the filter has
            // settled.  With the gate off, raim_mature is forced true and the output is
            // byte-identical.  Whole-sat bad_sats (network-wide contamination) are never
            // gated.  The EWMA is structurally available only when emit_ura, raim and
            // phase_leveling are all on: only then is the σ̂0-EWMA fed, so only then does the
            // update-count term apply.  Without emit_ura the EWMA is never updated and the
            // horizon alone decides (matching the RTB-6 sentinel and the URA-ramp lift rule).
            const bool raim_mature = !raim_maturity_gate || networkWarmupRampLifted(
                static_cast<long long>(n_emitted), raim_maturity_epochs, pl_warmup,
                static_cast<long long>(ura_ewma_updates), ura_on && raim_on && pl_on);
            std::map<std::pair<int, std::string>, int> next_arc_streak;  // K>1 only
            for (std::size_t k = 0; k < tbl.size(); ++k) {
                if (mp.interaction[k] / sigma0 <= raim_k) continue;
                const int sta = std::get<0>(tbl[k]);
                const std::string& sat = std::get<1>(tbl[k]);
                if (bad_sats.count(sat)) continue;   // caught wholesale; no double attribution
                if (!raim_mature) continue;   // no isolated-arc refloat before maturity (T2d)
                if (raim_arc_persist <= 1) {
                    bad_arcs.emplace_back(sta, sat);   // byte-identical immediate refloat
                    continue;
                }
                const auto akey = std::make_pair(sta, sat);
                const auto it = arc_spike_streak.find(akey);
                const int streak = (it != arc_spike_streak.end() ? it->second : 0) + 1;
                if (streak >= raim_arc_persist)
                    bad_arcs.emplace_back(sta, sat);   // persisted K epochs → refloat, drop streak
                else
                    next_arc_streak[akey] = streak;    // still building → carry forward
            }
            if (raim_arc_persist > 1) arc_spike_streak.swap(next_arc_streak);
            n_arc_refloat_epoch += static_cast<int>(bad_arcs.size());  // heartbeat churn metric (T2d)
        }

        if (attrib_on) {
            std::map<std::string, int> sat_n, sig_n;
            std::map<int, int> sta_n;
            for (const auto& c : cells) { ++sat_n[c.sat]; ++sig_n[c.sig]; ++sta_n[c.sta]; }
            auto emit = [&](const char* dim, const std::string& entity, double effect,
                            double prom, int n, int acted) {
                raim_attrib_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << dim << ',' << entity << ','
                    << fmt::format("{:.4f}", effect) << ',' << fmt::format("{:.2f}", prom) << ','
                    << n << ',' << fmt::format("{:.4f}", mp.overall) << ','
                    << fmt::format("{:.3f}", sigma0) << ',' << acted << '\n';
            };
            for (const auto& [sat, pr] : sat_prom)
                if (pr > kAttribReportProm)
                    emit("SAT", sat, mp.col_eff.at(sat), pr, sat_n[sat],
                         (attrib_act ? bad_sats.count(sat) : fset.count(sat)) ? 1 : 0);
            for (const auto& [sta, pr] : sta_prom)
                if (pr > kAttribReportProm)
                    emit("STA", stations[sta].name, mp.row_eff.at(sta), pr, sta_n[sta],
                         (attrib_act && bad_stations.count(sta)) ? 1 : 0);
            for (const auto& [sig, pr] : sig_prom)
                if (pr > kAttribReportProm)
                    emit("SIG", sig, sig_eff.at(sig), pr, sig_n[sig], 0);
            std::vector<std::size_t> order(mp.interaction.size());
            for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return std::abs(mp.interaction[a]) > std::abs(mp.interaction[b]);
            });
            int arc_logged = 0;
            for (std::size_t k : order) {
                const double pr = std::abs(mp.interaction[k]) / inter_mad;
                if (pr <= kAttribReportProm || arc_logged >= 20) break;
                const int sta = std::get<0>(tbl[k]);
                const std::string& sat = std::get<1>(tbl[k]);
                const int acted = attrib_act
                    ? ((mp.interaction[k] / sigma0 > raim_k || bad_sats.count(sat)
                        || bad_stations.count(sta)) ? 1 : 0)
                    : (fset.count(sat) ? 1 : 0);
                emit("ARC", stations[sta].name + "/" + sat, mp.interaction[k], pr, 1, acted);
                ++arc_logged;
            }
        }

        // Execute the attribution-driven refloat (whole sat, whole station or single arc,
        // per the decomposition).  Erasing local_carry and marking pending-reseed is the
        // same one-shot refloat the consensus path uses.
        // Event harness: a staged arc_erase runs the T4 path.  It is appended to bad_arcs,
        // so the injected erase and a real attribution execute the same code (carry
        // erase, iono-carry erase, pending-reseed, C3c queue); only its event row marks it
        // as injected.  It also opens the block on an epoch whose σ̂₀ never tripped,
        // which is what the harness is for.
        bool inject_arc_act = false;
        auto joinSet = [](const std::set<std::string>& v) {
            std::string o;
            for (const auto& x : v) o += (o.empty() ? "" : ";") + x;
            return o;
        };
        // Diagnostic sub-knob for Study 1 (docs/SPEC_event_harness.md SS9):
        // (station idx, sat) -> the injected mode.  Consulted only for arcs this
        // injector staged; a real attribution's arcs are absent from the map and
        // take the production path unchanged.
        std::map<std::pair<int, std::string>, std::string> inject_arc_mode;
        /// Band scope per injected arc (empty set => every signal, production).
        std::map<std::pair<int, std::string>, std::set<std::string>> inject_arc_sigs;
        std::vector<const InjectArcErase*> inject_arc_staged;
        if (ctx.inject.armed && !ctx.inject.arc_erase_now.empty()) {
            bool any_erase_mode = false;
            for (const auto& d : ctx.inject.arc_erase_now) {
                if (!d.isErase()) continue;   // free_epoch is the PASS-A drain's
                any_erase_mode = true;
                for (size_t i = 0; i < stations.size(); ++i)
                    if (stations[i].name == d.station && present[i]
                        && est_index_of[i] >= 0
                        && (phase_datum_prioritized || !stations[i].is_pivot)) {
                        bad_arcs.emplace_back(static_cast<int>(i), d.sat);
                        inject_arc_mode[{ static_cast<int>(i), d.sat }] = d.mode;
                        inject_arc_sigs[{ static_cast<int>(i), d.sat }] = d.signals;
                        inject_arc_staged.push_back(&d);
                        inject_arc_act = true;
                    }
            }
            if (any_erase_mode && !inject_arc_act)
                log->error("Network (fault inject): TOW {:.1f} arc_erase found no "
                           "eligible (station, satellite) — the station is absent, "
                           "un-indexed, or the legacy pivot.  The event will be "
                           "reported as NEVER FIRED at run-end.", se.time.tow);
        }
        if ((attrib_act || inject_arc_act)
            && (!bad_sats.empty() || !bad_stations.empty() || !bad_arcs.empty())) {
            const bool retap_on =
                config_.networkCycleSlipRelevelRetapStatistical();
            const double retap_var = config_.networkCycleSlipRelevelRetapVarM2();
            const int    retap_streak_max = config_.networkCycleSlipRelevelRetapStreakMax();
            // Corroborated keep (SPEC_lane_slip_response §3).  The wu93 lane-integer A/B
            // found both blanket policies wrong: blanket refloat (F1) costs ~29 points of
            // settled EWL at-own-integer against response-virgin chords, and blanket keep
            // (F3) costs a further 16 against F1 (the kept levels were wrong at flag time,
            // and var 0.25 anchors them).  The flagged population is heterogeneous, and
            // only per-arc evidence separates it.  Rule (the D5-v3 tri-state): keep only
            // when the pre-solve raw MW/GF pass measured this (station, sat) as
            // continuous, that is, measured and not stepped.  Unmeasured, stepped, no
            // evidence this epoch, or the qualification pass not run at all: erase,
            // exactly as without the retap (invariant #1: a slip response is never
            // suppressed on absence of evidence).
            const bool retap_corr =
                retap_on && config_.networkCycleSlipRelevelRetapCorroborate();
            auto retapEvidenceFlat = [&](int i, const std::string& sat,
                                         bool& was_stepped) -> bool {
                was_stepped = false;
                if (!slip_ep || !slip_ep->computed
                    || static_cast<std::size_t>(i) >= slip_ep->verdict.size())
                    return false;
                if (slip_ep->stepped.count({ i, sat })) { was_stepped = true; return false; }
                const auto& vmap = slip_ep->verdict[static_cast<std::size_t>(i)];
                const auto vit = vmap.find(sat);
                return vit != vmap.end() && vit->second.measured;
            };
            // ---- Band-scoped erase (§Z, docs/SPEC_event_harness.md) -------------
            // `min_keep_one`: an isolated-arc (statistical) erase frees only the bands
            // whose own post-fit normalized residual stands above the T4 gate, and
            // always spares the band with the smallest |r|/σ̂₀.  The survivor is the
            // line of sight's level anchor; Study 1 measured that keeping one is worth
            // a factor of 9 on the persistent level floor.
            //
            // Per-signal residual source: `cells`, built in Pass 1 with one row per
            // (station, satellite, signal) holding that carried ambiguity's max
            // standardized post-fit phase residual, the same number the median polish
            // consumes after `cell_max` collapses the signal axis.  Nothing is
            // re-derived here; the scope reads the value the attribution was computed
            // from, one aggregation step earlier.
            //
            // The consensus whole-satellite path (statistical_arc = false) is not
            // scoped, on purpose: agreement across ≥ min_sta stations is evidence about
            // the satellite, not about one arc's bands.
            const bool bscope_t4 =
                config_.getNetworkConfig().raim_erase_band_scope
                    == NetworkRaimEraseBandScope::MinKeepOne;
            std::map<std::pair<int, std::string>,
                     std::vector<std::pair<std::string, double>>> cell_sig_r;
            /// (station, sat) -> the band left pinned, for the event row's detail.
            std::map<std::pair<int, std::string>, std::string> bs_kept_by_arc;
            if (bscope_t4)
                for (const auto& c : cells)
                    cell_sig_r[{ c.sta, c.sat }].emplace_back(c.sig, c.r);
            // Band scope of one cell.  The rule is the pure function
            // networkRaimEraseBandScope (unit-tested); this lambda only looks the cell up.
            auto bandScope = [&](int i, const std::string& sat) -> RaimEraseBandScope {
                const auto it = cell_sig_r.find({ i, sat });
                if (it == cell_sig_r.end()) return {};
                return networkRaimEraseBandScope(it->second, raim_k * sigma0);
            };
            auto refloat_sat_at = [&](int i, const std::string& sat,
                                      bool statistical_arc) -> int {
                int n = 0;
                // Resolved once per (station, satellite) visit, above the per-signal
                // loop, so the pin is one decision for all bands (§Z).
                std::set<std::string> bs_erase;
                std::string           bs_keep;
                bool                  bs_on = false;
                if (bscope_t4 && statistical_arc) {
                    const RaimEraseBandScope bs = bandScope(i, sat);
                    bs_erase = bs.erase;
                    bs_keep  = bs.keep;
                    const bool over = bs.kept_over_gate;
                    if (bs.act) {
                        if (bs.single_band) {
                            // Single-band arc: skip the erase outright.  The level
                            // is suspect, but freeing it re-draws (ρ, I, N) from one
                            // epoch of code with nothing left to anchor them, which
                            // is the failure this scope exists to prevent.
                            ++ctx.counters.n_bscope_t4_single_band;
                            ++ctx.counters.n_bscope_t4_cells;
                            bs_kept_by_arc[{ i, sat }] = bs_keep + "(single)";
                            return 0;
                        }
                        bs_on = true;
                        ++ctx.counters.n_bscope_t4_cells;
                        ++ctx.counters.n_bscope_t4_kept;
                        if (over) ++ctx.counters.n_bscope_t4_kept_over_gate;
                        bs_kept_by_arc[{ i, sat }] =
                            bs_keep + (over ? "(over_gate)" : "");
                    }
                    // No cell rows for this (station, sat): the arc is not in the
                    // attribution pool this epoch.  Fall through to the unscoped
                    // erase: the scope has no evidence to act on, and withholding
                    // the response on missing evidence is the other failure mode.
                }
                // Study-1 sub-knob: production defaults unless this arc was
                // staged by the injector with a diagnostic mode.  Every flag
                // below is `true` on the production path, so an unarmed run
                // executes the same path as without the sub-knob.
                bool inj_iono = true, inj_reseed = true, inj_funnel = true;
                const std::set<std::string>* inj_sigs = nullptr;
                if (!inject_arc_mode.empty()) {
                    const auto mit = inject_arc_mode.find({ i, sat });
                    if (mit != inject_arc_mode.end()) {
                        inj_iono   = (mit->second != "erase_keep_iono");
                        inj_reseed = (mit->second != "erase_no_reseed");
                        inj_funnel = (mit->second != "erase_no_funnel");
                        const auto sit = inject_arc_sigs.find({ i, sat });
                        if (sit != inject_arc_sigs.end() && !sit->second.empty())
                            inj_sigs = &sit->second;
                    }
                }
                for (const auto& pa : ambPool(asm_res[i]))
                    if (pa.sat == sat
                        && !(inj_sigs && inj_sigs->count(pa.signal) == 0)
                        && !(bs_on && bs_erase.count(pa.signal) == 0)) {
                        if (bs_on) ++ctx.counters.n_bscope_t4_erased;
                        // Retap (SPEC_slip_relevel_walk §6): an isolated-arc refloat
                        // carries no measured delta, and its population is dominated by
                        // false alarms (21114 arc refloats against ~4500 slip-driven on
                        // the 60-sta campaign), so the arc's level is usually correct
                        // and erasing it injects error in steady state.  The retap keeps
                        // the level, loosens the confidence (var raised to at least
                        // retap_var; idempotent, since the bad_sats + bad_arcs double
                        // visit is reachable), clears the coupling regression (as the
                        // warm-resume empty-coupling degrade does), and lets the data
                        // decide: a real missed slip wins against the loose prior within
                        // an epoch or two, and an arc that re-trips
                        // relevel_retap_streak_max times, each within 120 s of the last,
                        // falls back to the erase.  Whole-sat consensus refloats
                        // (statistical_arc = false) keep the erase: a network event at
                        // ≥ min_sta stations has real evidence behind it.
                        if (retap_on && statistical_arc) {
                            const auto key = std::make_tuple(i, pa.sat, pa.signal);
                            auto it = local_carry.find(key);
                            if (it != local_carry.end()) {
                                bool corr_ok = true;
                                if (retap_corr) {
                                    bool was_stepped = false;
                                    corr_ok = retapEvidenceFlat(i, pa.sat, was_stepped);
                                    if (!corr_ok)
                                        ++(was_stepped
                                               ? ctx.counters.n_relevel_retap_corr_stepped
                                               : ctx.counters.n_relevel_retap_corr_unmeasured);
                                }
                                if (corr_ok) {
                                    const double now_s =
                                        se.time.week * 604800.0 + se.time.tow;
                                    auto& streak = ctx.carry.relevel_retap_streak[key];
                                    streak.second =
                                        (now_s - streak.first <= 120.0) ? streak.second + 1 : 1;
                                    streak.first = now_s;
                                    if (streak.second < retap_streak_max) {
                                        it->second.var = std::max(it->second.var, retap_var);
                                        it->second.coupling.clear();
                                        ctx.carry.relevel_retapped_prev.insert(key);
                                        ++ctx.counters.n_relevel_retap_kept;
                                        if (retap_corr)
                                            ++ctx.counters.n_relevel_retap_corr_kept;
                                        ++n;
                                        continue;   // level kept: no erase, no mark, no queue
                                    }
                                    ++ctx.counters.n_relevel_retap_escalated;
                                }
                                // Corroboration refused or streak escalated: fall
                                // through to the erase, so the arc takes the full
                                // refloat exactly as without the retap.
                            } else {
                                ++ctx.counters.n_relevel_retap_miss;
                            }
                        }
                        {
                            // With pd_held_basis armed, a T4 attribution flag stamps the
                            // arc, so the next election refuses it the S-basis for
                            // flag_quiet_ep epochs and, if it holds the basis, deposes it
                            // before any response acts on the column (RF-2 items 3 and
                            // 4).  The stamp is set here, at the flag, not at the
                            // response: the two are different edges.
                            if (ctx.knobs.pd_held_basis)
                                ctx.carry.amb_flag_ep[{static_cast<int>(i), pa.amb_key}] =
                                    static_cast<long long>(ctx.counters.n_loop_epochs);
                            const auto lane_lk = std::make_tuple(i, pa.sat, pa.signal);
                            const auto lane_lit = local_carry.find(lane_lk);
                            if (lane_lit != local_carry.end()) {
                                // Pre-erase capture for the lane observatory, T4 side
                                // (§7.6(d)); measure-only, consumed at the next harvest.
                                // The erase below is the existing actuator, unchanged.
                                if (config_.networkCycleSlipRelevelLaneSnap()) {
                                    ctx.carry.lane_t4_pre[lane_lk] = {
                                        lane_lit->second.value_abs,
                                        lane_lit->second.last_seen_s,
                                        RelevelEpochState::kCauseT4Attrib };
                                    ++ctx.counters.n_lane_t4_captured;
                                }
                                // The entry is recorded before the erase (RF-1 §7.4 D3):
                                // this is the class design §3.4 replaces with a bounded
                                // drop, and its variance before the event exists only here.
                                // Under the float_kf vehicle this is a bounded drop, not
                                // an erase (RF-1 step 4, §3.4).  An erase removes all
                                // information about the column; the anchor's w = 1e10 pin
                                // holds the phase bias, so the only shared parameter left
                                // to absorb the opened direction is the clock (R10
                                // attributed 88 % of a run's unsignalled satellite-clock
                                // steps at dt = 0 to this event).  A drop bounds what can
                                // move and keeps value_abs and the arc's identity (birth
                                // stamps, var_first, n_folds), so the next epoch
                                // re-couples against a stated prior instead of a first
                                // sighting.
                                if (ctx.knobs.amb_float_kf) {
                                    // The drop is taken at both floors, each mapped at this
                                    // line of sight's own elevation, the same mapping the
                                    // birth stash uses (RF-3a A1).  Both floors are 0 by
                                    // default, so the mapping matters only on a run that
                                    // arms a floor.
                                    const double e_pa = pa.elev_deg;
                                    const auto o_t4 = ambFloatDropFloored(
                                        lane_lit->second,
                                        ambFloatBirthFloor(ctx.knobs.amb_birth_var_floor,
                                                           e_pa, amb_elev_power_),
                                        ambFloatBirthFloor(amb_drop_floor_now_,
                                                           e_pa, amb_elev_power_),
                                        se.time.week * 604800.0 + se.time.tow,
                                        static_cast<long long>(
                                            ctx.counters.n_loop_epochs),
                                        ctx.knobs.amb_drop_decouple_always);
                                    if (o_t4.decoupled_idem)
                                        ++ctx.counters.n_amb_drop_decoupled_idem;
                                    if (o_t4.from_drop_floor)
                                        ++ctx.counters.n_amb_drop_floored;
                                    if (amb_drop_pre_gate_)
                                        ++ctx.counters.n_amb_drop_pre_gate;
                                    ambStateNoteDrop(ctx, se.time.week, se.time.tow,
                                                     stations[i].name, pa.sat, pa.signal,
                                                     lane_lit->second,
                                                     AmbDropCause::kT4Attrib);
                                    ++ctx.counters.n_amb_drop;
                                    ++ctx.counters.n_amb_drop_t4;
                                } else {
                                ambStateNoteErase(ctx, se.time.week, se.time.tow,
                                                  stations[i].name, pa.sat, pa.signal,
                                                  lane_lit->second, "t4_attrib");
                                local_carry.erase(lane_lit);
                                }
                                ++ctx.counters.n_cr_raim_attrib;   // census (C): T4 attribution refloat
                            }
                        }
                        // A T4 or consensus erase means the arc's level is not trusted,
                        // and the attribution cannot separate level error from
                        // carried-iono error (the T4 statistic reads their sum), so the
                        // LOS's iono carry is erased with it and the re-level re-draws
                        // both (queue 8b).  Idempotent across the per-signal loop; an arc
                        // kept by the retap above never reaches this line (kept level,
                        // kept iono carry).
                        if (inj_iono && ctx.knobs.iono_on &&
                            ctx.carry.iono_carry.erase({i, pa.sat}))
                            ++ctx.counters.n_iono_carry_erased_t4;
                        // Iono in the carried block (network.carry_iono_in_block,
                        // I-in-block): the same edge for the in-block vehicle, and its
                        // wired response under invariant #1.  The T4 statistic reads level
                        // error and carried-iono error as a sum, so an arc whose level is
                        // no longer trusted cannot vouch for the iono estimated jointly
                        // with it.  Erasing here makes the LOS re-draw both from data (its
                        // next harvest re-creates the entry with a fresh birth_s).
                        // Separate map and counter: the two carries are mutually exclusive
                        // vehicles, so a run's erase count is never split across them.
                        if (ctx.knobs.iono_blk &&
                            ctx.carry.iono_blk_carry.erase({i, pa.sat}))
                            ++ctx.counters.n_iono_blk_erased_t4;
                        // The receiver clock in the carried block (RX-CLK-in-block) has no
                        // T4 edge here, by a triaged decision rather than an omission.
                        // Invariant #1 requires a wired event response for every stiff
                        // time-linked quantity; this one's is the jump gate at the
                        // harvest, not the arc refloat.  A T4 refloat is a per-(station,
                        // satellite) arc event, while the receiver clock is station-wide
                        // and re-determined every epoch by every code and phase row of the
                        // station; erasing it on any one arc's refloat would erase the
                        // carry on most station-epochs of a slipped bed and leave the
                        // channel effectively unarmed.  A real clock displacement caused
                        // by a refloat appears as a one-step prediction innovation and is
                        // caught by network.carry_rx_clock_jump_k.
                        // Under carry_invariant this mark is bookkeeping, not the actuator:
                        // the producer forms every innovation against the linearization
                        // point and never writes the solution back, so moving x_lin leaves
                        // the reduced shared RHS b_red = u_s − N_sr N_rr⁻¹ u_r
                        // algebraically unchanged (SPEC_deweight_release_hygiene §7.2).  It
                        // is kept because it is the actuator on phase-leveling runs.
                        if (inj_reseed)
                            stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                        // The actuator under carry_invariant: queue for the next epoch's
                        // C3c funnel, which membership-excludes the arc from A_old so its
                        // chord is re-tapped from a diffuse prior.  Deferred because this
                        // epoch's funnel was consumed at SB_DDBUILD, before SB_RAIM.
                        if (inj_funnel && raim_ci_refloat) {
                            raim_refloat_pending.insert({i, pa.amb_key});
                            ++raim_ci_tallies.refloats_queued;
                        }
                        ++n;
                    }
                return n;
            };
            int n_ref = 0;
            for (const auto& sat : bad_sats)
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i] && est_index_of[i] >= 0
                        && (phase_datum_prioritized || !stations[i].is_pivot))
                        n_ref += refloat_sat_at(static_cast<int>(i), sat,
                                                /*statistical_arc=*/false);
            // (bad_stations is intentionally empty: the whole-station refloat path is
            //  disabled to avoid mass refloats from regional mis-attribution.)
            for (const auto& [sta, sat] : bad_arcs)
                n_ref += refloat_sat_at(sta, sat, /*statistical_arc=*/true);
            if (inject_arc_act) {
                // The injected pairs are the tail of bad_arcs (appended above).
                const std::size_t n_inj = inject_arc_staged.size();
                std::string modes;
                for (const InjectArcErase* d : inject_arc_staged) {
                    traces.emitEvent(se.time, "INJECT", "arc_erase", d->sat, d->station,
                                     std::nullopt, std::nullopt, "injected",
                                     fmt::format("t4_path;mode={};bands={};sigma0={:.3f};chords={}",
                                                 d->mode,
                                                 d->signals.empty() ? std::string("all")
                                                                    : joinSet(d->signals),
                                                 sigma0, n_ref));
                    ctx.inject.arc_erase_mode_acted[d->mode] += n_ref;
                    modes += (modes.empty() ? "" : ",") + d->mode;
                    const std::size_t k = d->ev_index;
                    if (k < ctx.inject.events.size() && !ctx.inject.onset_logged[k]) {
                        ++ctx.inject.n_applied[k];
                        ctx.inject.onset_logged[k] = true;
                        ++ctx.inject.fired_by_kind["arc_erase"];
                    }
                }
                ctx.inject.n_arc_erase_acted += n_ref;
                log->warn("Network (fault inject): TOW {:.1f} injected T4 erase on {} "
                          "arc(s) [mode={}] refloated {} carried chord(s) through the "
                          "ATTRIBUTION path (sigma0={:.3f}).",
                          se.time.tow, n_inj, modes, n_ref, sigma0);
            }
            attrib_acted = true;
            if (raim_ci_on) raim_ci_tallies.arcs_flagged += static_cast<long long>(bad_arcs.size());

            std::string detail;
            for (const auto& s : bad_sats)
                detail += (detail.empty() ? "" : ",") + std::string("sat:") + s;
            for (int s : bad_stations)
                detail += (detail.empty() ? "" : ",") + std::string("sta:") + stations[s].name;
            if (!bad_arcs.empty())
                detail += fmt::format("{}{} arc", detail.empty() ? "" : ",", bad_arcs.size());
            log->info("Network[RAIM-attrib]: TOW {:.1f} refloated {} carried ambiguity(ies) — "
                      "{} bad sat(s), {} bad station(s), {} isolated arc(s) "
                      "(σ̂0={:.2f}, gate {:.1f}) [{}]",
                      se.time.tow, n_ref, bad_sats.size(), bad_stations.size(),
                      bad_arcs.size(), sigma0, raim_k, detail);
            // One T4 event per acted entity (SAT with its column prominence; ARC keyed by
            // station).  The full per-entity attribution (including arc prominence) is in
            // network_raim_attrib.csv; only the refloated entities are logged here.
            if (events_trace.is_open()) {
                const std::string sd = fmt::format("sigma0={:.2f}", sigma0);
                for (const auto& s : bad_sats)
                    emitEvent("T4_attrib", "attrib_sat", s, "",
                              sat_prom.count(s) ? std::optional<double>(sat_prom.at(s))
                                                : std::nullopt,
                              raim_k, "refloat", sd);
                for (const auto& [sta, sat] : bad_arcs) {
                    // The kept band is part of the event's identity (§Z): a
                    // band-scoped arc is a different response from one erased
                    // whole, and the trace records which it was.
                    const auto kit = bs_kept_by_arc.find({ sta, sat });
                    emitEvent("T4_attrib", "attrib_arc", sat, stations[sta].name,
                              std::nullopt, raim_k, "refloat",
                              kit == bs_kept_by_arc.end()
                                  ? sd
                                  : sd + ";band_scope=min_keep_one;kept=" + kit->second);
                }
            }
        }
    }

    // --- "So bad" within-epoch adaptation trigger (detection + log) ---------------
    // When σ̂0 exceeds the trigger, the joint solve is contaminated enough that emitting
    // it would be wrong.  The within-epoch adaptation (network.raim_within_epoch) drops
    // the dominant culprit(s) and re-solves the epoch (a beginEpoch replay of the scatter
    // minus the culprit) in the solve glue, before this seam runs, and only on carry_warm
    // epochs.  This block fires when this seam's σ̂0 still exceeds the trigger: it logs a
    // warning and a T2 event, and the epoch is emitted anyway.  It is not carry_warm-gated,
    // so it also fires on epochs where no adaptation ran.
    if (config_.networkRaimWithinEpoch() && sigma0 > config_.networkRaimSigma0Trigger()) {
        log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} STILL > trigger {:.2f} after "
                  "adaptation — residual contamination the culprit-drop could not isolate "
                  "(emitted anyway; inspect network_raim_attrib.csv).",
                  se.time.tow, sigma0, config_.networkRaimSigma0Trigger());
        emitEvent("T2_within_epoch", "emitted_contaminated", "", "", sigma0,
                  config_.networkRaimSigma0Trigger(), "emit_anyway", "");
    }

    // Per-satellite consensus refloat, only when attribution is not driving the action.
    if (!attrib_act && !flagged.empty()) {
        int n_refloat = 0;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0
                || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
            for (const auto& pa : ambPool(asm_res[i]))
                if (fset.count(pa.sat)) {
                    // Remove the contaminated carry outright (under float_kf, the
                    // bounded drop; see the attribution site).  The harvest has just
                    // re-stashed this ambiguity's contaminated value into local_carry;
                    // erasing it means the next epoch finds no prior to inject
                    // (cit == end, so no addStationPrior), and the ambiguity refloats
                    // from data without depending on the deferred pending-reseed skip
                    // path.  The mark is also set (redundant; cleared by the next stash).
                    {
                        // Flag stamp (RF-2 items 3 and 4); see the attribution site.
                        if (ctx.knobs.pd_held_basis)
                            ctx.carry.amb_flag_ep[{static_cast<int>(i), pa.amb_key}] =
                                static_cast<long long>(ctx.counters.n_loop_epochs);
                        const auto lane_lk =
                            std::make_tuple(static_cast<int>(i), pa.sat, pa.signal);
                        const auto lane_lit = local_carry.find(lane_lk);
                        if (lane_lit != local_carry.end()) {
                            // Pre-erase capture (§7.6(d)); see the attribution site.
                            if (config_.networkCycleSlipRelevelLaneSnap()) {
                                ctx.carry.lane_t4_pre[lane_lk] = {
                                    lane_lit->second.value_abs,
                                    lane_lit->second.last_seen_s,
                                    RelevelEpochState::kCauseT4Consens };
                                ++ctx.counters.n_lane_t4_captured;
                            }
                            // Entry record and bounded drop (RF-1 §7.4 D3, RF-1 step 4);
                            // see the attribution site.
                            if (ctx.knobs.amb_float_kf) {
                                // Elevation-mapped floors (RF-3a A1); see the attribution site.
                                const double e_pa = pa.elev_deg;
                                const auto o_t3 = ambFloatDropFloored(
                                    lane_lit->second,
                                    ambFloatBirthFloor(ctx.knobs.amb_birth_var_floor,
                                                       e_pa, amb_elev_power_),
                                    ambFloatBirthFloor(amb_drop_floor_now_,
                                                       e_pa, amb_elev_power_),
                                    se.time.week * 604800.0 + se.time.tow,
                                    static_cast<long long>(
                                        ctx.counters.n_loop_epochs),
                                    ctx.knobs.amb_drop_decouple_always);
                                if (o_t3.decoupled_idem)
                                    ++ctx.counters.n_amb_drop_decoupled_idem;
                                if (o_t3.from_drop_floor)
                                    ++ctx.counters.n_amb_drop_floored;
                                if (amb_drop_pre_gate_)
                                    ++ctx.counters.n_amb_drop_pre_gate;
                                ambStateNoteDrop(ctx, se.time.week, se.time.tow,
                                                 stations[i].name, pa.sat, pa.signal,
                                                 lane_lit->second,
                                                 AmbDropCause::kT3Consensus);
                                ++ctx.counters.n_amb_drop;
                                ++ctx.counters.n_amb_drop_t3;
                            } else {
                            ambStateNoteErase(ctx, se.time.week, se.time.tow,
                                              stations[i].name, pa.sat, pa.signal,
                                              lane_lit->second, "t3_consensus");
                            local_carry.erase(lane_lit);
                            }
                            ++ctx.counters.n_cr_raim_consensus;   // census (C): T3 consensus refloat
                        }
                    }
                    // Bookkeeping under carry_invariant (see refloat_sat_at above); the
                    // actuator there is the deferred funnel queue.
                    stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                    if (raim_ci_refloat) {
                        raim_refloat_pending.insert({static_cast<int>(i), pa.amb_key});
                        ++raim_ci_tallies.refloats_queued;
                    }
                    ++n_refloat;
                }
        }
        std::string flist;
        for (size_t e = 0; e < flagged.size(); ++e) { if (e) flist += ","; flist += flagged[e]; }
        log->info("Network[RAIM]: TOW {:.1f} refloated {} carried ambiguity(ies) on {} "
                  "satellite(s) [{}] — normalized post-fit residual > {:.1f} at >= {} "
                  "stations (σ̂0={:.2f}; a pre-processor-missed slip leaked into the shared "
                  "clock, caught by network redundancy).",
                  se.time.tow, n_refloat, flagged.size(), flist, raim_k, raim_min_sta, sigma0);
        // One T3 event per consensus-refloated sat.  statistic = the per-sat max
        // normalized post-fit residual (max r_max / σ̂0 over its stations), from amb_stats.
        if (events_trace.is_open()) {
            std::map<std::string, double> t3_norm;
            for (const auto& a : amb_stats) {
                const double sn = (sigma0 > 0.0) ? a.r_max / sigma0 : a.r_max;
                double& m = t3_norm[a.sat];
                m = std::max(m, sn);
            }
            for (const auto& s : flagged)
                emitEvent("T3_raim", "consensus_refloat", s, "",
                          t3_norm.count(s) ? std::optional<double>(t3_norm.at(s))
                                           : std::nullopt,
                          raim_k, "refloat",
                          fmt::format("sigma0={:.2f};min_sta={}", sigma0, raim_min_sta));
        }
    }
    (void)attrib_acted;
}

} // namespace GnssProcessor

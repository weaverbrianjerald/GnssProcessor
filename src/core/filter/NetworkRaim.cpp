/**
 * @file NetworkRaim.cpp
 * @brief Implementation of @ref networkPostFitRaim — the post-fit network RAIM
 *        pass relocated VERBATIM (dedent-only + one documented substitution) out of
 *        Application::runNetwork at decomposition INC-9 (2026-07-22).
 *
 * The body below is a byte-copy of the former SB_RAIM block
 * (ApplicationNetwork.cpp lines 7090-7523), dedented 8 spaces, with the single
 * substitution `plan.shared_datum` -> `shared_datum` (the run TU held the shared
 * datum inside NetworkSbasis::Plan; this seam takes only the Datum it reads).  The
 * alias preamble reproduces the run-TU alias block so the relocated code reads the
 * original names.  See NetworkRaim.hpp for the ordering contract.
 */

#include "filter/NetworkRaim.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.knobs / ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"              // config_ getters
#include "filter/NetworkMdb.hpp"              // Baarda MDB rider on B2's r_i (SPEC_redundancy_census)
#include "filter/NetworkRobustStats.hpp"      // medianPolish2Way / robustProminence / medianCopy / MedianPolish
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
// §Z BAND-SCOPED T4 ERASE — the rule (docs/SPEC_event_harness.md §Z)
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
    // INTERACTION (net of the row and column effects), so a per-signal residual
    // can sit under k·σ̂₀ while the arc still stands out.  Erase everything but
    // the pin — never suppress the response on absence of per-band evidence.
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
    const bool   raim_maturity_gate   = ctx.knobs.raim_maturity_gate;   // T2d: gate isolated-arc refloats on maturity
    const int    raim_maturity_epochs = ctx.knobs.raim_maturity_epochs;  // T2d: maturity horizon
    const bool   raim_redun    = ctx.knobs.raim_redun; // B2: Baarda per-row r_i normalization of the RAIM residual
    const bool   raim_redun_trace = ctx.knobs.raim_redun_trace; // §7.1: COMPUTE r_i for the census without USING it
    // docs/SPEC_redundancy_census.md MDB-INC-0: the MDB rider on B2's r_i.  NOT
    // part of the relocated body — a measure-only addition that reads the (w, r_i)
    // pairs the Pass-1 row walk already visits.  Both conjuncts matter: mdb_on is
    // the master, mdb_from_raim says THIS feed site is the armed one.
    const bool   mdb_on        = ctx.knobs.mdb_on && ctx.knobs.mdb_from_raim;
    const double mdb_delta0    = baardaDelta0(config_.dataSnoopingCritical());
    // docs/SPEC_prior_residual_census.md §14.2: the DATA SIDE of the
    // constraint-aware global variance component.  Also NOT part of the relocated
    // body — a measure-only rider reading the same (w, v, r_i) triples the Pass-1
    // walk already visits, and nothing in this seam reads a single number back.
    // globalOn() already ANDs the rider with a live census, so this is one bool.
    const bool   s0g_on        = prires.globalOn();
    const bool   clk_mon_couple = ctx.knobs.clk_mon_couple;  // also refloat RAIM-flagged sats' clocks (opt-in)
    const bool   ura_on        = ctx.knobs.ura_on;                  // B1: emit per-sat URA quality channel
    const double ura_alpha     = ctx.knobs.ura_alpha;             // B1: s0 EWMA smoothing
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   trace_flag_rates = ctx.knobs.trace_flag_rates;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;

    // --- RAIM POOL RE-HOST (docs/SPEC_raim_pool_rehost.md) ---------------------------
    // Every walk below used to key on R.pl_amb, which is the PHASE-LEVELING CARRY's
    // record list.  On a Card-4 lineage that list is empty by construction (its fill
    // site sits under `do_amb_carry = (…) && !carry_invariant`), so this entire seam —
    // the σ̂0 pool, the consensus test, the median-polish attribution, the MDB rider,
    // the constraint-aware global σ̂0 data side and the L13 station-residual trace — ran
    // over nothing.  `raim_ci_on` fills the carry-neutral R.qc_amb census instead; the
    // selector below is the single point of substitution for all of them.
    const bool raim_ci_on      = ctx.knobs.raim_ci_on;
    const bool raim_ci_refloat = ctx.knobs.raim_ci_refloat;
    // Prefer the census when it exists; fall through to the legacy pool otherwise, so a
    // phase-leveling run is byte-identical (qc_amb is never allocated when OFF).
    auto ambPool = [&](const StationAssembly& R) -> const std::vector<PlAmb>& {
        return R.qc_amb.empty() ? R.pl_amb : R.qc_amb;
    };

    auto& local_carry = ctx.carry.local_carry;
    auto& raim_refloat_pending = ctx.carry.raim_refloat_pending;
    // QUEUE item 4 R2/R4 (SPEC_t3_null_derivation §6): both measure-only, both
    // loader-guaranteed to require network.raim.  R2 additionally needs per-row
    // r_i, obtained through the same solve-neutral rowRedundancy read the census
    // uses (the disjunct below gains staf_on).
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
    auto& raim_redun_out       = traces.raim_redun_trace;   // §7.1 per-row r_i census

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
    // Prominence-attribution input (collected iff attribution is LOGGED or DRIVES the
    // action): one cell per (station, satellite, signal) carried-ambiguity residual.
    const bool attrib_on   = raim_attrib_trace.is_open();       // log the decomposition
    const bool attrib_act  = config_.networkRaimAttribution();  // let it drive the refloat
    // B8/T4 (P1-4): also build the attribution table under trace_flag_rates so the T4
    // flag-rate is MEASURABLE on a clean baseline with ONLY network.trace_flag_rates on —
    // the documented empirical-α workflow — without needing raim_attribution or the
    // attribution trace.  The median polish then runs read-only (no refloat unless
    // attrib_act, no trace unless attrib_on), feeding only the fr_attrib counter; byte-
    // identical when trace_flag_rates is off (this disjunct is then absent).
    const bool need_attrib = attrib_on || attrib_act || trace_flag_rates;
    struct Cell { int sta; std::string sat, sig; double r; };
    std::vector<Cell> cells;
    // R2: per-station factor rows, collected during Pass 1 and reduced AFTER σ̂₀
    // exists (the trim bar is k·σ̂₀, which is not known until the pool closes).
    std::vector<std::vector<StationFactorRow>> staf_rows;
    if (staf_on) staf_rows.resize(stations.size());
    for (size_t i = 0; i < stations.size(); ++i) {
        // Prioritized: keep the former pivot — its non-anchored arcs are in
        // R.pl_amb (anchored/pinned keys already excluded at carry build).
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
        Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
        // SPEC_t3_null_derivation §7.1: the CENSUS also needs r_i, and needs it WITHOUT
        // the standardization consuming it — arming raim_redundancy_normalize is
        // solve-affecting (it drops rows at the floor), so a diagnostic that required it
        // could not be run on a validated posture.  Computing row_r is a pure read of the
        // already-factored normal equations, so the disjunct below is solve-NEUTRAL: the
        // value is used only where `raim_redun` itself is tested.
        if (raim_redun || raim_redun_trace || staf_on) {
            try {
                row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                          R.A_s, R.A_r, R.w, shared_datum);
            } catch (...) { row_r.resize(0); }
        }
        // R2: this station's factor is computable only with a full r_i vector — a
        // rowRedundancy throw leaves the station "unavailable" this epoch (the
        // trace's −1 sentinel path; never 0).
        const bool staf_here = staf_on && row_r.size() == R.A_r.rows();
        const bool redun_trace_here = raim_redun_out.is_open();
        // --- MDB rider (docs/SPEC_redundancy_census.md MDB-INC-0) ------------
        // Every row this loop is about to visit already has (w, r_i) in hand, so
        // its minimal detectable bias costs one division.  The visited mask exists
        // because the loop below is keyed on AMBIGUITY COLUMNS, not on rows: a row
        // touched by two carried ambiguities would otherwise be binned twice and
        // the join against the census would drift by exactly those duplicates.
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
        // Its OWN visited mask, deliberately not shared with the MDB one: MDB
        // needs `raim_redun` to exist at all, this rider does not (it degrades to
        // r ≡ 1 and FLAGS the epoch), so the two populations are not the same set
        // and one mask would silently give whichever card ran second a subset.
        // The mask itself is mandatory for the identical reason MDB's is: the
        // loop below is keyed on AMBIGUITY COLUMNS, not on rows, so a row touched
        // by two carried ambiguities would be counted twice — inflating BOTH sums
        // and leaving the ratio perfectly plausible.
        const bool s0g_redun_here = raim_redun && row_r.size() == R.A_r.rows();
        std::vector<char> s0g_seen;
        if (s0g_on) s0g_seen.assign(static_cast<std::size_t>(R.A_r.rows()), 0);
        std::vector<char> staf_seen;   // R2: row de-dup (column-keyed loop, MDB lesson)
        if (staf_here) staf_seen.assign(static_cast<std::size_t>(R.A_r.rows()), 0);
        // Largest measurement-standardized post-fit phase residual PER SATELLITE at this
        // station (a carried ambiguity's phase rows carry +1 on its A_r column; uncombined).
        std::map<std::string, double> sat_rmax;
        // B9-in-block (network.carry_zwd_in_block) — SPEC_amb_iono_split_overcount §7 item 4,
        // MEASURED not guessed.  The pool admits a row by |A_r(row, amb_col)| > 0.5, a test
        // written for DATA phase rows (which carry exactly +1 on their ambiguity column).
        // A whitened block row is one row of L⁻¹, so the ZWD's block row carries non-zero
        // coefficients on the ambiguity columns below it and CAN clear that bar — i.e. a
        // carried-prior row can enter a post-fit fault pool built for observations.  This
        // card does NOT change the rule; it counts the interaction so the next card decides
        // on data.  The ZWD's row is the coupling row whose primary local column is the
        // station's ZWD column (cpl_row_local_cols is in append order from cpl_row_start).
        Eigen::Index zwd_blk_row = -1;
        if (R.pl_zwd_in_block && R.cpl_row_start >= 0 && R.pl_zwd.col >= 0)
            for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                if (R.cpl_row_local_cols[rr] == R.pl_zwd.col) {
                    zwd_blk_row = R.cpl_row_start + static_cast<Eigen::Index>(rr);
                    break;
                }
        bool zwd_row_pooled = false;   // once per station-epoch (the column-keyed loop can revisit it)
        for (const auto& pa : pool) {
            if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
            double r_max = 0.0;
            for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
                if (row == zwd_blk_row && !zwd_row_pooled) {
                    zwd_row_pooled = true;
                    ++ctx.counters.n_raim_pool_zwd_rows;
                }
                const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                // MDB is fed HERE, ahead of the uncheckable `continue` below: a row
                // RAIM drops from its σ̂0 pool because r_i ≤ 1e-6 is precisely the
                // row whose MDB is +infinity, and that is the number this card
                // exists to publish.  Binning only the rows that survive the drop
                // would report the network as perfectly checkable by construction.
                if (mdb_here && !mdb_seen[static_cast<std::size_t>(row)]) {
                    mdb_seen[static_cast<std::size_t>(row)] = 1;
                    mdb_feed->add(pa.sat, NetRowClass::ObsPhase, w, row_r(row));
                }
                // The global variance component is fed HERE, ahead of the
                // uncheckable `continue` below, for the reason the identity
                // itself supplies: E[w·v²] = r_i, so an r_i ≈ 0 row contributes
                // ~0 to the numerator AND ~0 to the denominator and cannot bias
                // the ratio — while DROPPING it would delete a real row from
                // Σr_data and report the pool as more redundant than it is.
                // (Same conclusion as MDB's, reached from the arithmetic rather
                // than from what the card publishes.)
                if (s0g_on && !s0g_seen[static_cast<std::size_t>(row)]) {
                    s0g_seen[static_cast<std::size_t>(row)] = 1;
                    prires.addDataRow(w, resid(row),
                                      s0g_redun_here ? row_r(row) : 1.0,
                                      /*r_is_approx=*/!s0g_redun_here);
                }
                double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                // R2 factor feed: HERE, ahead of the uncheckable `continue`, for the
                // s0g reason (E[w·v²] = r_i — an r_i ≈ 0 row contributes ~0 to both
                // sums and cannot bias the ratio, while dropping it would overstate
                // the station's redundancy).  Guarded by its own seen-mask: the loop
                // is keyed on ambiguity COLUMNS, so a row touched by two carried
                // ambiguities would otherwise inflate both sums (the MDB/s0g lesson).
                // The trim statistic is |v|·√w always — the B2 division is a pool
                // convention, not part of the §7.5 factor.
                if (staf_here && !staf_seen[static_cast<std::size_t>(row)]) {
                    staf_seen[static_cast<std::size_t>(row)] = 1;
                    staf_rows[i].push_back(
                        {w * resid(row) * resid(row), row_r(row), r});
                }
                // §7.1 census.  Emitted HERE — above the uncheckable `continue` below —
                // for the same reason the MDB and σ̂₀-global feeds are: the rows the
                // normalization DROPS are precisely the ones worth seeing, and a census
                // that only recorded survivors would report a population that cannot
                // have a floor problem by construction.  One row per (sta, sat, SIGNAL,
                // row); the signal is what the cell's max destroys.
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
                if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
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

    // Robust a-posteriori variance factor for this epoch, network-wide.  Floored at 1
    // so a clean network is no more sensitive than the nominal stochastic model
    // (σ̂0→1 ⇒ identical to the plain standardized residual); when the whole network
    // misfits at cm, σ̂0 rises and the normalized residuals stay O(1) — so RAIM stops
    // firing on ambient misfit and flags only what stands out ABOVE it.
    double sigma0 = 1.0;
    if (!r_pool.empty()) {
        std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
        sigma0 = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
    }
    // Pool re-host telemetry: σ̂0 on this lineage has NEVER been measured — with the pool
    // empty, r_pool was empty too and sigma0 never left the 1.0 initializer above.  Record
    // it only over epochs with a live pool, so the mean is not diluted by that sentinel.
    if (raim_ci_on && !r_pool.empty()) {
        raim_ci_tallies.sigma0_max = std::max(raim_ci_tallies.sigma0_max, sigma0);
        raim_ci_tallies.sigma0_sum += sigma0;
        ++raim_ci_tallies.sigma0_n;
    }
    // B1: fold the network-wide σ̂0 into the URA EWMA (scales next epoch's emitted
    // URA — the post-emit ordering means the scale lands one epoch later).
    // RTB-7(b): the update COUNT feeds the quality-ramp lift rule (the
    // "EWMA has stabilized" half); read only while the warmup floor is armed.
    if (ura_on) {
        ura_sigma0_ewma = ura_alpha * sigma0 + (1.0 - ura_alpha) * ura_sigma0_ewma;
        ++ura_ewma_updates;
    }

    // --- QUEUE item 4 R2: the DECLARED per-station variance factor -------------
    // (SPEC_t3_null_derivation §6/§7.5.)  Measure-only: nothing below feeds any
    // denominator, gate or response — absorption is R1, HELD behind the measured
    // per-station iono error it would bury (§7.8.2).  Runs post-σ̂₀ because the
    // trim bar is k·σ̂₀.  The seam only runs on accepted epochs, so the EWMA
    // freezes across withholds by construction.
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
                // Declaration warmup: no edge before the EWMA has real memory
                // (10 updates ≈ 5 min at 30 s — a fresh station must not alarm
                // off its seed value).
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
    std::map<std::string, int> sat_cell_stations;  // R4: per-sat cell count (unconditional)
    for (const auto& a : amb_stats) {
        const double s_norm = a.r_max / sigma0;
        const bool   spiked = (s_norm > raim_k);
        if (rate_hygiene) ++sat_cell_stations[a.sat];
        // B8/T3 cell.  R4: an s_raw exactly 0 cell (carried ambiguity with no phase
        // row this epoch) is structurally untestable — it can never flag, and
        // counting it as an opportunity deflates the reported rate (§4: 1.67 %).
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
        // R4: the shipped opportunity count is CONDITIONAL (only satellites that
        // already spiked somewhere), so implied_alpha compared a conditional rate
        // against an unconditional α (§1).  Under hygiene the opportunity is the
        // UNCONDITIONAL population: every satellite with cells at ≥ raim_min_sta
        // stations this epoch (the population structurally able to reach consensus).
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

    // A consensus-flagged satellite's slip leaked into its SHARED clock (that is why the
    // innocent stations spiked) — refloat the carried clock alongside the ambiguity.
    if (clk_mon_couple)
        for (const auto& s : fset) clock_refloat_sats.insert(s);
    // P0-2: consensus-flagged sats contaminated the emitted correction — feed the
    // retro-bump independent of the couple opt-in.
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
    // Robust 2-way median polish of the (station × satellite) standardized-residual
    // table separates a bad SATELLITE (whole column lights up), a bad STATION (whole
    // row), and an isolated ARC (interaction); a per-signal marginal on the polished
    // leftover flags a bad SIGNAL.  One pass over the residual cells already in hand —
    // no re-solve, unlike the user-side iterative snoop.  Always LOGGED when the
    // attribution trace is open; when network.raim_attribution is on it also DRIVES
    // the refloat (bad sat → network-wide, bad station → that station's arcs, isolated
    // arc → just that (station,sat)) instead of the per-satellite consensus.
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

        // B8/T4 (P1-4): attribution flag-rate on the per-satellite median-polish
        // column effect, using the SAME criterion the acting path (bad_sats) uses —
        // σ̂0 above the within-epoch trigger AND effect/σ̂0 above raim_k — so the rate
        // is measurable on a clean baseline whether or not raim_attribution is acting.
        if (trace_flag_rates) {
            const double trig_a = config_.networkRaimSigma0Trigger();
            for (const auto& [sat, eff] : mp.col_eff) {
                (void)sat;
                ++fr_attrib.opp;
                if (sigma0 > trig_a && sigma0 > 0.0 && eff / sigma0 > raim_k) ++fr_attrib.flag;
            }
            // §9.7.10: the ARC sub-path's criterion rate.  The SAT line above is
            // σ̂0-trigger-conjoined and therefore structurally silent on a clean
            // epoch — while the ARC sub-path (the one that actually refloats
            // isolated slips) fires at ANY σ̂0.  Without this line the summary
            // reads "T4: N opp, 0 flag" on runs where T4 refloated arcs — the
            // §0 error class (an instrument that does not record the acting
            // sub-path).  Criterion only (pre persistence/maturity gates),
            // mirroring how the SAT line mirrors the bad_sats criterion.
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
        // sat/station were NOT already caught (no double attribution).
        std::set<std::string> bad_sats;
        std::set<int>         bad_stations;   // deliberately never populated — see below
        std::vector<std::pair<int, std::string>> bad_arcs;
        if (attrib_act) {
            // Wholesale SATELLITE refloat only when the network is GENUINELY contaminated
            // (σ̂0 above the within-epoch trigger).  At a robust-clean epoch (σ̂0 ≈ 1) a high
            // column effect is a median-polish artifact of a satellite visible across a
            // regional station cluster, NOT a network-wide slip — refloating its carriers
            // everywhere would wipe good accumulated precision and destabilize (the root of
            // the 305 24h ~4 h lock-in).  The isolated-arc path below still refloats real
            // single slips at ANY σ̂0.
            if (sigma0 > config_.networkRaimSigma0Trigger())
                for (const auto& [sat, eff] : mp.col_eff)
                    if (eff / sigma0 > raim_k) bad_sats.insert(sat);
            // Refloat the carried clock of an attribution-flagged (network-wide) satellite
            // too — an isolated ARC is a local slip and does NOT implicate the shared clock,
            // so only whole-satellite culprits feed the clock refloat.
            if (clk_mon_couple)
                for (const auto& s : bad_sats) clock_refloat_sats.insert(s);
            // P0-2: attribution whole-satellite culprits contaminated the emitted
            // correction — feed the retro-bump independent of the couple opt-in.
            // (Isolated arcs are LOCAL slips that do not implicate the shared per-sat
            //  correction, so they are excluded — mirroring the clock-refloat scoping.)
            if (retro_feed)
                for (const auto& s : bad_sats) retro_flag_sats.insert(s);
            // Bad-STATION wholesale refloat is DELIBERATELY DISABLED: a regional cluster of
            // stations lighting up on ONE shared satellite event inflates their median-polish
            // ROW effects, so this path mass-refloated dozens of innocent stations at once
            // (78 stations / 3977 carriers at TOW 273150 in the 24 h run) → cascade →
            // divergence.  A genuinely bad station is handled by the within-epoch station-
            // exclude (σ̂0 > trigger) or surfaces as isolated arcs; a whole-station carrier
            // wipe is never worth the mis-attribution risk.  bad_stations stays empty.
            // Persistence gate (network.raim_arc_persist_epochs, K): an isolated arc is
            // refloated only after it trips the interaction gate K CONSECUTIVE epochs.  A real
            // slip persists as an integer step until refloated → trips every epoch → clears K
            // and refloats; a heavy-tail / model-error spike does not repeat on the same arc →
            // never reaches K → is starved.  K=1 (default) takes the immediate path below and is
            // byte-identical.  For K>1 we rebuild the streak map from THIS epoch's flagged set so
            // any arc not flagged now resets to 0 (absence ⇒ non-consecutive), and an arc that
            // reaches K refloats and is dropped from the carry (must re-persist K epochs for the
            // next refloat — a persistent arc refloats at most once per K epochs).
            // T2d: filter-maturity latch for the isolated-arc refloat gate — mirrors the
            // warmup URA ramp (emitted-epoch horizon AND σ̂0-EWMA update count, the count
            // waived when the EWMA is structurally unavailable).  Isolated arcs are the
            // self-sustaining warmup churn driver (2→1063/epoch); suppress them until the
            // filter has settled.  Forced true when the gate is OFF ⇒ byte-identical.
            // Whole-sat bad_sats (network-wide contamination) are NEVER gated.
            // ewma_structurally_available == (emit_ura && raim && phase_leveling): only
            // then is the σ̂0-EWMA a fed signal, so only then does the update-count term
            // apply; without emit_ura the EWMA is never updated and the horizon alone
            // decides (matching the RTB-6 sentinel + the URA-ramp lift rule).
            const bool raim_mature = !raim_maturity_gate || networkWarmupRampLifted(
                static_cast<long long>(n_emitted), raim_maturity_epochs, pl_warmup,
                static_cast<long long>(ura_ewma_updates), ura_on && raim_on && pl_on);
            std::map<std::pair<int, std::string>, int> next_arc_streak;  // K>1 only
            for (std::size_t k = 0; k < tbl.size(); ++k) {
                if (mp.interaction[k] / sigma0 <= raim_k) continue;
                const int sta = std::get<0>(tbl[k]);
                const std::string& sat = std::get<1>(tbl[k]);
                if (bad_sats.count(sat)) continue;   // caught wholesale; no double attribution
                if (!raim_mature) continue;   // T2d: suppress isolated-arc refloat pre-maturity
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
            n_arc_refloat_epoch += static_cast<int>(bad_arcs.size());  // T2d: heartbeat churn metric
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

        // Execute the attribution-driven refloat (surgical: whole sat / whole station /
        // single arc, per the decomposition).  Erasing local_carry + pending-reseed is
        // the same one-shot refloat the consensus path uses.
        // EVENT HARNESS: a staged arc_erase runs THE T4 path — it is appended to
        // bad_arcs so the injected erase and a real attribution execute the same
        // code (carry erase + iono-carry erase + pending-reseed + C3c queue), and
        // is marked as injected only in its event row.  It also OPENS the block on
        // an epoch whose σ̂₀ never tripped: that is the point of the harness.
        bool inject_arc_act = false;
        auto joinSet = [](const std::set<std::string>& v) {
            std::string o;
            for (const auto& x : v) o += (o.empty() ? "" : ";") + x;
            return o;
        };
        // Study-1 DIAGNOSTIC sub-knob (docs/SPEC_event_harness.md SS9):
        // (station idx, sat) -> the injected mode.  Consulted ONLY for arcs this
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
            // SPEC_lane_slip_response §3: corroborated keep.  The wu93 lane-integer
            // A/B measured BOTH blanket policies wrong — blanket refloat (F1) costs
            // ~29 points of settled EWL at-own-integer vs response-virgin chords,
            // and blanket keep (F3) costs a further 16 vs F1 (the kept levels were
            // wrong at flag time and var 0.25 anchors them).  The flagged population
            // is heterogeneous; only per-arc EVIDENCE separates it.  Rule (the
            // D5-v3 tri-state): keep ONLY when the pre-solve raw MW/GF pass
            // POSITIVELY measured this (station, sat) as continuous — measured AND
            // not stepped.  Unmeasured, stepped, no-evidence-this-epoch, or the
            // qualification pass not run at all ⇒ the erase, exactly as without
            // the retap (invariant #1: never suppress a slip response on absence
            // of evidence).
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
            // ---- §Z BAND-SCOPED ERASE (docs/SPEC_event_harness.md) -------------
            // `min_keep_one`: an ISOLATED-ARC (statistical) erase frees only the
            // bands whose OWN post-fit normalized residual stands above the T4
            // gate, and ALWAYS spares the band with the smallest |r|/σ̂₀ — the
            // survivor is the line of sight's level anchor, and Study 1 measured
            // that keeping one is worth 9× on the persistent level floor.
            //
            // Per-signal residual source: `cells`, built in Pass 1 as one row per
            // (station, satellite, SIGNAL) carrying that carried-ambiguity's max
            // standardized post-fit phase residual — the SAME number the median
            // polish consumes after `cell_max` collapses the signal axis away.
            // Nothing is re-derived here; the scope reads the value the
            // attribution itself was computed from, one aggregation step earlier.
            //
            // The CONSENSUS whole-satellite path (statistical_arc = false) is
            // deliberately NOT scoped: a ≥ min_sta cross-station agreement is real
            // evidence about the satellite, not about one arc's bands.
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
            // Adjudicate one cell: the RULE is the pure
            // networkRaimEraseBandScope (unit-tested); this only looks the cell up.
            auto bandScope = [&](int i, const std::string& sat) -> RaimEraseBandScope {
                const auto it = cell_sig_r.find({ i, sat });
                if (it == cell_sig_r.end()) return {};
                return networkRaimEraseBandScope(it->second, raim_k * sigma0);
            };
            auto refloat_sat_at = [&](int i, const std::string& sat,
                                      bool statistical_arc) -> int {
                int n = 0;
                // §Z: resolved once per (station, satellite) visit, above the
                // per-signal loop, so the pin is one decision and not a race
                // between bands.
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
                            // Single-band arc — skip the erase outright.  The level
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
                    // No cell rows for this (station, sat) — the arc is not in the
                    // attribution pool this epoch.  Fall through to the unscoped
                    // erase: the scope has no evidence to act on, and refusing the
                    // response on missing evidence is the other failure mode.
                }
                // Study-1 sub-knob: production defaults unless THIS arc was
                // staged by the injector with a diagnostic mode.  Every flag
                // below is `true` on the production path, so an unarmed run's
                // instruction stream is the pre-card one.
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
                        // SPEC_slip_relevel_walk §6, retap piece: an ISOLATED-ARC
                        // refloat carries NO measured delta and its population is
                        // FA-dominated (21114 arc refloats vs ~4500 slip-driven on
                        // the 60-sta campaign) — the arc's level is overwhelmingly
                        // CORRECT, so erasing it was the steady-state injection.
                        // KEEP the level, SET the confidence loose (idempotent —
                        // the bad_sats+bad_arcs double-visit is reachable), clear
                        // the coupling regression (the warm-resume empty-coupling
                        // degrade precedent), and let the data adjudicate: a real
                        // missed slip wins against the loose prior within an epoch
                        // or two, and an arc re-tripping relevel_retap_streak_max
                        // times inside 120 s falls back to the erase.  Whole-sat
                        // CONSENSUS refloats (statistical_arc = false) keep the
                        // strong erase — a ≥min_sta network event has real
                        // evidence behind it.
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
                                        continue;   // level kept — no erase, no mark, no queue
                                    }
                                    ++ctx.counters.n_relevel_retap_escalated;
                                }
                                // corroboration refused (or streak escalated): fall
                                // through to the erase — the un-kept arc takes the
                                // full refloat exactly as without the retap.
                            } else {
                                ++ctx.counters.n_relevel_retap_miss;
                            }
                        }
                        {
                            const auto lane_lk = std::make_tuple(i, pa.sat, pa.signal);
                            const auto lane_lit = local_carry.find(lane_lk);
                            if (lane_lit != local_carry.end()) {
                                // §7.6(d) lane-observatory T4-side pre-erase capture
                                // (measure-only; consumed at the NEXT harvest).  The
                                // erase below is the existing actuator, untouched.
                                if (config_.networkCycleSlipRelevelLaneSnap()) {
                                    ctx.carry.lane_t4_pre[lane_lk] = {
                                        lane_lit->second.value_abs,
                                        lane_lit->second.last_seen_s,
                                        RelevelEpochState::kCauseT4Attrib };
                                    ++ctx.counters.n_lane_t4_captured;
                                }
                                local_carry.erase(lane_lit);
                                ++ctx.counters.n_cr_raim_attrib;   // census (C): T4 ATTRIBUTION refloat
                            }
                        }
                        // QUEUE 8b: a T4/consensus erase means the arc's LEVEL is not
                        // trusted, and the attribution cannot separate level error from
                        // carried-iono error (the T4 statistic reads their sum) — erase
                        // the LOS's iono carry with it so the re-level re-draws both.
                        // Idempotent across the per-signal loop; a retap-KEPT arc above
                        // never reaches this line (kept level ⇒ kept iono carry).
                        if (inj_iono && ctx.knobs.iono_on &&
                            ctx.carry.iono_carry.erase({i, pa.sat}))
                            ++ctx.counters.n_iono_carry_erased_t4;
                        // BOOKKEEPING, not the actuator on the Card-4 path: the producer
                        // forms every innovation against the linearization point and never
                        // writes the solution back, so moving x_lin leaves the reduced
                        // shared RHS b_red = u_s − N_sr N_rr⁻¹ u_r algebraically unchanged
                        // (SPEC_deweight_release_hygiene §7.2).  Kept because it IS the
                        // actuator on the phase-leveling lineage.
                        if (inj_reseed)
                            stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                        // THE actuator under carry_invariant: queue for the next epoch's
                        // C3c funnel, which membership-excludes the arc from A_old so its
                        // chord is re-tapped from a diffuse prior.  Deferred because the
                        // funnel was consumed at SB_DDBUILD, long before SB_RAIM.
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
            // (bad_stations is intentionally empty — the whole-station refloat path is
            //  disabled to avoid the regional mis-attribution mass-refloat.)
            for (const auto& [sta, sat] : bad_arcs)
                n_ref += refloat_sat_at(sta, sat, /*statistical_arc=*/true);
            if (inject_arc_act) {
                // The injected pairs are the TAIL of bad_arcs (appended above).
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
            // T4: one event per ACTED entity (SAT with its column prominence; ARC keyed
            // by station).  Full per-entity attribution (incl. arc prominence) is in
            // network_raim_attrib.csv; here we log only the entities that were refloated.
            if (events_trace.is_open()) {
                const std::string sd = fmt::format("sigma0={:.2f}", sigma0);
                for (const auto& s : bad_sats)
                    emitEvent("T4_attrib", "attrib_sat", s, "",
                              sat_prom.count(s) ? std::optional<double>(sat_prom.at(s))
                                                : std::nullopt,
                              raim_k, "refloat", sd);
                for (const auto& [sta, sat] : bad_arcs) {
                    // §Z: the kept band is part of the event's identity — an arc
                    // that was band-scoped is a DIFFERENT response from one that
                    // was erased whole, and the trace must say which it was.
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
    // When σ̂0 exceeds the trigger the joint solve is contaminated enough that emitting
    // it would be wrong.  The full adaptation drops the dominant culprit(s) and RE-SOLVES
    // this epoch (a beginEpoch replay of the scatter minus the culprit) to re-condition;
    // that core-solve change is staged.  For now we DETECT + arm it so the trigger rate
    // and the drop targets can be validated against the attribution first.
    if (config_.networkRaimWithinEpoch() && sigma0 > config_.networkRaimSigma0Trigger()) {
        log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} STILL > trigger {:.2f} after "
                  "adaptation — residual contamination the culprit-drop could not isolate "
                  "(emitted anyway; inspect network_raim_attrib.csv).",
                  se.time.tow, sigma0, config_.networkRaimSigma0Trigger());
        emitEvent("T2_within_epoch", "emitted_contaminated", "", "", sigma0,
                  config_.networkRaimSigma0Trigger(), "emit_anyway", "");
    }

    // Per-satellite consensus refloat — only when attribution is NOT driving the action.
    if (!attrib_act && !flagged.empty()) {
        int n_refloat = 0;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0
                || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
            for (const auto& pa : ambPool(asm_res[i]))
                if (fset.count(pa.sat)) {
                    // Drop the contaminated carry OUTRIGHT.  The harvest above
                    // just re-stashed this ambiguity's (contaminated) value into
                    // local_carry; erasing it means the next epoch finds no prior
                    // to inject (cit == end → no addStationPrior), so the ambiguity
                    // re-floats from data — a direct, unambiguous refloat that does
                    // not depend on the deferred pending-reseed skip path.  The mark
                    // is also set (belt-and-suspenders / cleared by the next stash).
                    {
                        const auto lane_lk =
                            std::make_tuple(static_cast<int>(i), pa.sat, pa.signal);
                        const auto lane_lit = local_carry.find(lane_lk);
                        if (lane_lit != local_carry.end()) {
                            // §7.6(d) pre-erase capture — see the attribution site.
                            if (config_.networkCycleSlipRelevelLaneSnap()) {
                                ctx.carry.lane_t4_pre[lane_lk] = {
                                    lane_lit->second.value_abs,
                                    lane_lit->second.last_seen_s,
                                    RelevelEpochState::kCauseT4Consens };
                                ++ctx.counters.n_lane_t4_captured;
                            }
                            local_carry.erase(lane_lit);
                            ++ctx.counters.n_cr_raim_consensus;   // census (C): T3 CONSENSUS refloat
                        }
                    }
                    // Bookkeeping on the Card-4 path (see refloat_sat_at above); the real
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
        // T3: one event per consensus-refloated sat.  statistic = the per-sat max
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

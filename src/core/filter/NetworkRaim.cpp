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
#include "filter/NetworkMdb.hpp"              // Baarda MDB rider on B2's r_i (SPEC_network_mdb_reliability)
#include "filter/NetworkRobustStats.hpp"      // medianPolish2Way / robustProminence / medianCopy / MedianPolish
#include "filter/StateManager.hpp"            // stations[i].sm->markAmbiguityPendingReseed
#include "logging/NetworkRunTraces.hpp"       // trace streams + emitEvent
#include "products/NetworkStateSnapshot.hpp"  // networkWarmupRampLifted

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
                        NetworkPriorResidual& prires) {
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
    // docs/SPEC_network_mdb_reliability.md INC-0: the MDB rider on B2's r_i.  NOT
    // part of the relocated body — a measure-only addition that reads the (w, r_i)
    // pairs the Pass-1 row walk already visits.  Both conjuncts matter: mdb_on is
    // the master, mdb_from_raim says THIS feed site is the armed one.
    const bool   mdb_on        = ctx.knobs.mdb_on && ctx.knobs.mdb_from_raim;
    const double mdb_delta0    = baardaDelta0(config_.dataSnoopingCritical());
    // docs/SPEC_sigma0_constraint_aware.md §2: the DATA SIDE of the
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
        if (raim_redun) {
            try {
                row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                          R.A_s, R.A_r, R.w, shared_datum);
            } catch (...) { row_r.resize(0); }
        }
        // --- MDB rider (docs/SPEC_network_mdb_reliability.md INC-0) ------------
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
        // --- constraint-aware global σ̂₀, data side (SPEC_sigma0_constraint_aware) --
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
        // Largest measurement-standardized post-fit phase residual PER SATELLITE at this
        // station (a carried ambiguity's phase rows carry +1 on its A_r column; uncombined).
        std::map<std::string, double> sat_rmax;
        for (const auto& pa : pool) {
            if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
            double r_max = 0.0;
            for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
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

    // Pass 2: normalize by σ̂0 and take the cross-station consensus per satellite.
    std::map<std::string, int> sat_flag_stations;  // satellite -> #stations whose normalized residual spiked
    struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bool spiked; };
    std::vector<RaimDetail> raim_details;           // per (station, sat); built only when tracing
    for (const auto& a : amb_stats) {
        const double s_norm = a.r_max / sigma0;
        const bool   spiked = (s_norm > raim_k);
        if (trace_flag_rates) { ++fr_raim_cell.opp; if (spiked) ++fr_raim_cell.flag; }  // B8/T3 cell
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
        fr_raim_sat.opp  += static_cast<long>(sat_flag_stations.size());
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
        if (attrib_act && (!bad_sats.empty() || !bad_stations.empty() || !bad_arcs.empty())) {
            auto refloat_sat_at = [&](int i, const std::string& sat) -> int {
                int n = 0;
                for (const auto& pa : ambPool(asm_res[i]))
                    if (pa.sat == sat) {
                        local_carry.erase(std::make_tuple(i, pa.sat, pa.signal));
                        // BOOKKEEPING, not the actuator on the Card-4 path: the producer
                        // forms every innovation against the linearization point and never
                        // writes the solution back, so moving x_lin leaves the reduced
                        // shared RHS b_red = u_s − N_sr N_rr⁻¹ u_r algebraically unchanged
                        // (SPEC_deweight_release_hygiene §7.2).  Kept because it IS the
                        // actuator on the phase-leveling lineage.
                        stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                        // THE actuator under carry_invariant: queue for the next epoch's
                        // C3c funnel, which membership-excludes the arc from A_old so its
                        // chord is re-tapped from a diffuse prior.  Deferred because the
                        // funnel was consumed at SB_DDBUILD, long before SB_RAIM.
                        if (raim_ci_refloat) {
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
                        n_ref += refloat_sat_at(static_cast<int>(i), sat);
            // (bad_stations is intentionally empty — the whole-station refloat path is
            //  disabled to avoid the regional mis-attribution mass-refloat.)
            for (const auto& [sta, sat] : bad_arcs) n_ref += refloat_sat_at(sta, sat);
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
                for (const auto& [sta, sat] : bad_arcs)
                    emitEvent("T4_attrib", "attrib_arc", sat, stations[sta].name,
                              std::nullopt, raim_k, "refloat", sd);
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
                    local_carry.erase(std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
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

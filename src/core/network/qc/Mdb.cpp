/**
 * @file Mdb.cpp
 * @brief Baarda reliability numbers (MDB) on the network path; see
 *        Mdb.hpp for the design
 *        (docs/SPEC_redundancy_census.md).
 *
 * Measure-only.  Nothing in this translation unit writes a weight, a state or a
 * decision; it reads @f$(\sigma_i, r_i)@f$ pairs the host loops already hold,
 * divides once, and emits aggregates, counters and a reporting event stream.
 *
 * ## Why the aggregates are scaled at flush time, not at feed time
 *
 * @f$\hat\sigma_{0,f}@f$ is a positive per-family constant for the epoch, and a
 * positive scaling commutes with every order statistic:
 * @f$\mathrm{med}(c\,x) = c\,\mathrm{med}(x)@f$, and likewise for any quantile
 * and the maximum.  Aggregating the formal pool and multiplying the three numbers
 * at the end is therefore exact.  It is also the only ordering that works: the
 * P1-4 pass that measures @f$\hat\sigma_{0,f}@f$ runs in SB_FRAMEDIAG, after the
 * SB_RAIM feed site, so scaling at feed time would apply the previous epoch's
 * inflation to this epoch's rows.
 */

#include "network/qc/Mdb.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

// ===========================================================================
//  Small value types
// ===========================================================================

void NetworkMdbBin::reset() {
    mdb.clear();                 // capacity retained on purpose (see the header)
    n_rows = n_inf = n_over = 0;
    worst = worst_r = worst_sigma = 0.0;
    worst_station.clear();
    worst_sat.clear();
}

void NetworkMdbEpoch::reset(double new_tow) {
    for (int s = 0; s < kNetMdbConstellations; ++s)
        for (int c = 0; c < kNetDataClasses; ++c) bin[s][c].reset();
    cand.clear();
    rows_raim = rows_census = 0;
    census_rows = census_unchk = 0;
    skipped_weight = 0;
    fed_raim = fed_census = false;
    tow = new_tow;
}

void NetworkMdbStationFeed::finish() {
    for (int c = 0; c < kNetDataClasses; ++c) {
        const Worst& w = worst_[static_cast<std::size_t>(c)];
        if (!(w.mdb > 0.0)) continue;          // this class saw no finite row here
        acc_.cand.push_back(NetworkMdbCandidate{station_, w.sat, c,
                                                w.mdb, w.r, w.sigma});
    }
}

// ===========================================================================
//  Testable pieces (no run state, no I/O)
// ===========================================================================

NetworkMdbStats networkMdbStats(std::vector<double>& v) {
    NetworkMdbStats s;
    const std::size_t n = v.size();
    if (n == 0) return s;
    // Median: the upper median for even n, the convention of the other robust
    // statistics in the network producer (stationPrefitCensus, the RAIM sigma0
    // pool, the clock QC pools), so every median column uses the same one.
    const std::size_t im = n / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(im), v.end());
    s.med = v[im];
    // p95, nearest-rank: index ceil(0.95 n) - 1, clamped.  Nearest-rank rather
    // than an interpolating definition because the pool is a census of rows, not
    // a sample from a continuum, and because a one-row bin then reports that row.
    std::size_t ip = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    ip = (ip == 0) ? 0 : ip - 1;
    if (ip >= n) ip = n - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(ip), v.end());
    s.p95 = v[ip];
    s.max = *std::max_element(v.begin(), v.end());
    return s;
}

NetworkMdbScale networkMdbFamilyScale(int cls, long long pf_rows,
                                      long long pf_code_rows, long long pf_phase_rows,
                                      double s0_code, double s0_phase) {
    NetworkMdbScale f;
    if (pf_rows <= 0) { f.src = "absent_no_pf"; return f; }
    double s0 = 0.0;
    if (cls == static_cast<int>(NetRowClass::ObsCode)) {
        if (pf_code_rows <= 0) { f.src = "absent_pool_empty"; return f; }
        s0 = s0_code;
        f.src = "postfit_code";
    } else if (cls == static_cast<int>(NetRowClass::ObsPhase)) {
        if (pf_phase_rows <= 0) { f.src = "absent_pool_empty"; return f; }
        s0 = s0_phase;
        f.src = "postfit_phase";
    } else {
        // obs_other / iono_sh_row: the P1-4 pass has no pool for them at all.
        f.src = "absent_no_pool_for_class";
        return f;
    }
    if (!(s0 > 0.0) || !std::isfinite(s0)) {
        f.src = "absent_sigma0_zero";       // the free-ambiguity phase case
        return f;
    }
    f.have = true;
    f.sigma0 = s0;
    // A deflating scale (sigma0 < 1) is reported, never clamped, following the
    // prior-residual census's rule: clamping would report an optimistically
    // calibrated family as calibrated, and the deflation is the measurement.  It
    // is tagged because a scaled MDB below the formal one reads as a tighter
    // reliability guarantee, and on the phase family the meaning is the reverse:
    // the post-fit phase pool has almost no dispersion because each phase row is
    // fitted by its own free ambiguity, which is the same fact as "these rows
    // carry no redundancy".  A small scaled phase MDB is not evidence that a fault
    // of that size would be detected.
    if (s0 < 1.0) {
        f.src = (cls == static_cast<int>(NetRowClass::ObsCode))
                    ? "postfit_code_deflating" : "postfit_phase_deflating";
    }
    return f;
}

// ===========================================================================
//  File-local helpers
// ===========================================================================

namespace {

/// @ref networkMdbFamilyScale bound to this epoch's P1-4 scalars.
NetworkMdbScale familyScale(const NetworkRunContext& ctx, int cls) {
    const auto& c = ctx.carry;
    return networkMdbFamilyScale(cls, c.ep_pf_rows, c.ep_pf_code_rows,
                                 c.ep_pf_phase_rows, c.ep_sigma0_pf_code,
                                 c.ep_sigma0_pf_phase);
}

/// Format a double for the CSV, or an empty field when it is unavailable.
/// Blank, never 0 and never the formal value, so that "unavailable" and "measured
/// zero" have different encodings (the epoch-status convention).
std::string num(bool have, double v) {
    return have ? fmt::format("{:.6e}", v) : std::string();
}

}  // namespace

// ===========================================================================
//  The flush (INC-1)
// ===========================================================================

void networkMdbFlush(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const EpochEvents::Census& census_ev = ep.events.census;
    // ---- End of the bindings ----
    net::LosTable& los_rows = ctx.entities.los;   // written
    if (!ctx.knobs.mdb_on) return;   // the driver's gate
    // Alias preamble (seam contract): the body reads the driver's names.
    const auto& nc      = config_.getNetworkConfig();
    auto& acc           = ctx.carry.mdb;
    auto& q             = ctx.counters.mdb;
    const double event_m = ctx.knobs.mdb_event_m;
    const long long holdoff = ctx.knobs.mdb_event_holdoff;

    const auto t0 = std::chrono::steady_clock::now();
    ++q.passes;
    // The flush is the epoch clock for the per-cell event holdoff.  It advances on
    // every pass, including empty ones, so a holdoff configured in epochs counts
    // epochs rather than epochs on which something was fed.
    const long long seq = ctx.counters.mdb_seq++;

    // Not this epoch's accumulator ⇒ neither feed site ran.  That is a legal
    // state (the census is strided; RAIM needs a warm carry) and differs from
    // "ran and found nothing"; q.passes against q.epochs records the difference.
    if (acc.tow != se.time.tow || (!acc.fed_raim && !acc.fed_census)) {
        q.wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }

    ++q.epochs;
    q.rows_raim   += acc.rows_raim;
    q.rows_census += acc.rows_census;
    q.rows        += acc.rows_raim + acc.rows_census;
    q.rows_skipped += acc.skipped_weight;

    // ---- the aggregates and the trace row per (constellation x class) ------
    for (int s = 0; s < kNetMdbConstellations; ++s) {
        for (int c = 0; c < kNetDataClasses; ++c) {
            NetworkMdbBin& b = acc.bin[s][c];
            if (b.n_rows == 0) continue;
            q.rows_inf  += b.n_inf;
            q.rows_over += b.n_over;
            const NetworkMdbStats st = networkMdbStats(b.mdb);
            const bool finite_any = !b.mdb.empty();
            if (finite_any) {
                if (b.worst > q.max_mdb) {
                    q.max_mdb = b.worst;
                    q.max_mdb_id = fmt::format(
                        "{}/{}/{} @tow {:.1f} r={:.3e} sigma={:.3e}",
                        b.worst_station,
                        b.worst_sat.empty() ? "?" : b.worst_sat,
                        rowClassName(static_cast<NetRowClass>(c)), se.time.tow,
                        b.worst_r, b.worst_sigma);
                }
                const double lo = *std::min_element(b.mdb.begin(), b.mdb.end());
                if (!q.min_seen || lo < q.min_mdb) { q.min_seen = true; q.min_mdb = lo; }
            }
            const NetworkMdbScale f = familyScale(ctx, c);
            if (f.have) {
                ++q.bins_scaled;
                if (f.sigma0 > q.max_scale) q.max_scale = f.sigma0;
                if (f.sigma0 < 1.0) {
                    ++q.bins_deflating;
                    if (!q.min_scale_seen || f.sigma0 < q.min_scale) {
                        q.min_scale_seen = true;
                        q.min_scale = f.sigma0;
                        q.min_scale_id = fmt::format("{}/{} @tow {:.1f}",
                                                     netMdbConstChar(s),
                                                     rowClassName(static_cast<NetRowClass>(c)),
                                                     se.time.tow);
                    }
                }
            } else {
                ++q.bins_unscaled;
            }
            if (traces.mdb_trace.is_open()) {
                traces.mdb_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << netMdbConstChar(s) << ','
                    << rowClassName(static_cast<NetRowClass>(c)) << ','
                    << b.n_rows << ',' << b.n_inf << ','
                    << static_cast<long long>(b.mdb.size()) << ',' << b.n_over << ','
                    << num(finite_any, st.med) << ','
                    << num(finite_any, st.p95) << ','
                    << num(finite_any, st.max) << ','
                    << num(f.have, f.sigma0) << ',' << f.src << ','
                    << num(finite_any && f.have, st.med * f.sigma0) << ','
                    << num(finite_any && f.have, st.p95 * f.sigma0) << ','
                    << num(finite_any && f.have, st.max * f.sigma0) << ','
                    << b.worst_station << ',' << b.worst_sat << ','
                    << num(finite_any, b.worst_r) << ','
                    << num(finite_any, b.worst_sigma) << '\n';
            }
        }
    }

    // ---- the reporting event stream (per (station x class) worst row) ------
    // A reporting threshold, not a gate: nothing downstream reads mdb_event_m,
    // and the event's action column reads "observe".
    if (event_m > 0.0) {
        for (const NetworkMdbCandidate& cd : acc.cand) {
            if (!(cd.mdb > event_m)) continue;
            const char* cls = rowClassName(static_cast<NetRowClass>(cd.cls));
            // The line of sight's holdoff clock of the class (the station by NAME, O3).
            const std::optional<LosId> hid = net::losOfName(ctx.entities, cd.station, cd.sat);
            const long long* last =
                hid ? net::get(ctx.entities.los, *hid, &net::LosState::events, std::string(cls),
                               &net::LosEventState::mdb_event_last)
                    : nullptr;
            if (last != nullptr && holdoff > 0 && (seq - *last) < holdoff) {
                ++q.events_held;
                continue;
            }
            if (hid)
                net::put(los_rows, *hid, &net::LosState::events, std::string(cls),
                         &net::LosEventState::mdb_event_last) = seq;
            ++q.events;
            traces.emitEvent(se.time, "MDB", "mdb_exceedance", cd.sat, cd.station,
                             cd.mdb, event_m, "observe",
                             fmt::format("class={};r={:.4e};sigma_m={:.4e};"
                                         "delta0={:.4f}",
                                         cls, cd.r, cd.sigma,
                                         baardaDelta0(config_.dataSnoopingCritical())));
        }
    }

    // ---- the uncheckable join (one row domain, two instruments) ------------
    // Evaluable only on an epoch the census swept: its per-epoch scalars are
    // stamped, and a non-sweep epoch leaves them blank rather than repeating a
    // stale sweep's numbers.  The census's `uncheckable` uses the policy floor
    // network.redundancy_eps and MDB's infinity uses the arithmetic floor 1e-6, so
    // the infinity count is a subset and is not compared for equality.  What must
    // agree exactly is this seam's independent recount of the census's two
    // numbers over the same rows.
    if (acc.fed_census && census_ev.redun_ep_tow == se.time.tow) {
        ++q.join_checks;
        const long long cen_rows  = census_ev.ep_redun_rows;
        const long long cen_unchk = census_ev.ep_uncheckable_rows;
        if (acc.census_rows != cen_rows || acc.census_unchk != cen_unchk) {
            ++q.join_mismatch;
            const std::string msg = fmt::format(
                "tow {:.1f}: MDB counted {} data row(s) / {} below eps={:g}; the "
                "census counted {} / {}",
                se.time.tow, acc.census_rows, acc.census_unchk, nc.redundancy_eps,
                cen_rows, cen_unchk);
            if (q.join_first_mismatch.empty()) q.join_first_mismatch = msg;
            if (!ctx.counters.mdb_join_warned) {
                ctx.counters.mdb_join_warned = true;
                log->warn("Network (MDB): the uncheckable JOIN does not reconcile — {}. "
                          "One number, two instruments: a disagreement is a bug in the "
                          "row domain of one of them, not a finding about the data.  "
                          "Do not grade either instrument on this run until it is "
                          "resolved.", msg);
            }
        }
    }

    acc.reset(-1.0);   // consumed; the next feed re-stamps
    q.wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ===========================================================================
//  Run-end telemetry (standing invariant #4)
// ===========================================================================

void networkMdbRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                      const std::shared_ptr<spdlog::logger>& log) {
    const auto& nc = config_.getNetworkConfig();
    const auto& q  = ctx.counters.mdb;
    const double delta_0 = baardaDelta0(config_.dataSnoopingCritical());

    log->info("Network (MDB): {} flush pass(es), {} of them carrying rows; {} row(s) "
              "binned ({} from the RAIM pool, {} from the census sweep), {} skipped "
              "for an unusable weight.  delta_0 = {:.4f} (k = {:g}, beta = 0.20).  "
              "{:.1f} ms total wall in the flush (the per-row division rides the host "
              "loops and is not separable).",
              q.passes, q.epochs, q.rows, q.rows_raim, q.rows_census,
              q.rows_skipped, delta_0, config_.dataSnoopingCritical(), q.wall_ms);

    if (q.rows > 0) {
        log->info("Network (MDB) INC-1: {} row(s) UNCHECKABLE (MDB = inf at r <= {:g}, "
                  "{:.3f}% of rows) and {} FINITE row(s) above the {:g} m reporting "
                  "threshold ({} event(s) emitted, {} suppressed by the {}-pass "
                  "per-cell holdoff).  Best-checked finite row {:.4g} m; WORST finite "
                  "row {:.4g} m at {}.",
                  q.rows_inf, kBaardaRedundancyFloor,
                  100.0 * static_cast<double>(q.rows_inf) / static_cast<double>(q.rows),
                  q.rows_over, nc.mdb_event_m, q.events, q.events_held,
                  ctx.knobs.mdb_event_holdoff,
                  q.min_seen ? q.min_mdb : 0.0, q.max_mdb,
                  q.max_mdb_id.empty() ? std::string("(none finite)") : q.max_mdb_id);
        // Reporting rule 1 (see the header).  A run whose scaled column was never
        // populated has not measured its formal/scaled margin, and the formal
        // numbers above are the optimistic ones; the log states this next to them.
        if (q.bins_scaled > 0) {
            log->info("Network (MDB): {} of {} aggregate bin(s) carried an empirical "
                      "scale (largest sigma0_f = {:.3f}), so mdb_scaled = mdb_formal x "
                      "sigma0_f is populated there.  The formal/scaled GAP is this "
                      "run's honesty margin: D5 measured the formal sigma 10-30x "
                      "optimistic, and MDB scales linearly with it.",
                      q.bins_scaled, q.bins_scaled + q.bins_unscaled, q.max_scale);
            // A sigma0 below 1 makes the scaled MDB smaller than the formal one,
            // which reads as a tighter guarantee.  On the phase family the meaning
            // is the reverse: that pool has almost no post-fit dispersion because
            // each phase row is fitted by its own free ambiguity, which is the
            // same fact as "these rows carry no redundancy".  Reported, never
            // clamped (the prior-residual census's rule: the deflation is the
            // measurement), and warned.
            if (q.bins_deflating > 0)
                log->warn("Network (MDB): {} of those {} bin(s) scaled DOWNWARD "
                          "(sigma0_f < 1; deepest {:.4e} on {}), so their mdb_scaled "
                          "is SMALLER than mdb_formal.  Those rows are tagged "
                          "`*_deflating` in network_mdb.csv and must NOT be quoted as "
                          "a tightened reliability guarantee: on the phase family a "
                          "near-zero post-fit sigma0 is the SAME FACT as the "
                          "uncheckable count above (a free ambiguity fits its own "
                          "row exactly), not evidence that small faults are "
                          "detectable there.  Read mdb_formal and n_inf together on "
                          "any bin carrying that tag.",
                          q.bins_deflating, q.bins_scaled, q.min_scale,
                          q.min_scale_id.empty() ? std::string("(unnamed)")
                                                 : q.min_scale_id);
        } else
            log->warn("Network (MDB): NOT ONE aggregate bin carried an empirical scale "
                      "— every mdb_scaled column is blank.  Formal MDB inherits the "
                      "formal-sigma optimism (D5: 10-30x on this data), so the numbers "
                      "above are the OPTIMISTIC ones and must not be quoted to a "
                      "consumer as a reliability guarantee.  Arm "
                      "network.trace_postfit_sigma0 to populate the scaled variant.");
    }

    // The join.
    if (q.join_checks > 0) {
        if (q.join_mismatch == 0)
            log->info("Network (MDB): the uncheckable join RECONCILED on all {} "
                      "census epoch(s) — the MDB row domain and the redundancy "
                      "census's row domain are the same set.", q.join_checks);
        else
            log->warn("Network (MDB): the uncheckable join FAILED on {} of {} census "
                      "epoch(s).  First disagreement: {}.  One of the two instruments "
                      "is walking the wrong rows.",
                      q.join_mismatch, q.join_checks, q.join_first_mismatch);
    } else if (ctx.knobs.redun_census_on) {
        log->info("Network (MDB): the census-join check never had an epoch to run on "
                  "(no sweep completed), so the row-domain reconciliation is UNTESTED "
                  "this run — not passed.");
    }

    // --- invariant #4: distinguish "ran and found nothing" from "never ran" --
    if (q.passes == 0)
        log->warn("Network (MDB): ARMED but the flush ran on ZERO epochs — the wiring "
                  "never executed.  Nothing in this run is evidence about MDB.");
    else if (q.rows == 0)
        log->warn("Network (MDB): ARMED, the flush ran on {} epoch(s) and ZERO rows "
                  "were binned.  Both feed sites were inert: the RAIM pool needs "
                  "network.raim + network.raim_redundancy_normalize + a WARM carry, "
                  "and the census sweep needs an epoch matching "
                  "network.redundancy_census_stride.  This is a MISWIRE, not a "
                  "network in which every row is checkable.", q.passes);
    else if (q.rows_inf == 0)
        log->info("Network (MDB): {} row(s) binned and NOT ONE was uncheckable — every "
                  "row this run folded carries a finite minimal detectable bias.  That "
                  "is a genuine negative result about the topology, and it is also the "
                  "state in which the infinity count cannot move, so do not read it as "
                  "validation of the infinity path.", q.rows);
    if (q.rows > 0 && q.rows_raim == 0 && ctx.knobs.mdb_from_raim)
        log->warn("Network (MDB): the RAIM feed site is armed but contributed ZERO "
                  "rows.  Its domain is the post-fit RAIM Pass-1 walk, which needs "
                  "(a) a WARM carry — i.e. network.phase_leveling with its warmup "
                  "elapsed (phase_leveling={}) — and (b) a non-empty per-station "
                  "`pl_amb` list.  MEASURED CAUSE on this lineage: `pl_amb` is built "
                  "only when network.carry_invariant is OFF "
                  "(StationCarry.cpp, `do_amb_carry`), because the DD "
                  "invariant carry SUPERSEDES the phase-leveling ambiguity carry as a "
                  "solve input — so with carry_invariant={} the post-fit RAIM pool is "
                  "empty and network.raim_redundancy_normalize is inert with it.  That "
                  "is a property of the lineage, not of this feed: on a "
                  "carry_invariant run the redundancy census is the ONLY r_i source, "
                  "and every row above came from it.",
                  ctx.knobs.pl_on ? "on" : "off",
                  config_.networkCarryInvariant() ? "on" : "off");
    if (q.rows > 0 && q.rows_census == 0 && ctx.knobs.redun_census_on)
        log->warn("Network (MDB): the redundancy census is armed but contributed ZERO "
                  "rows to MDB — no sweep completed this run (check the census's own "
                  "run-end line).  Every row above came from the RAIM pool.");
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: Baarda MDB reliability.
int networkMdbRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // --- Baarda reliability numbers / MDB run-end telemetry -------------------
    // (docs/SPEC_redundancy_census.md §11.4).  The aggregates have to distinguish
    // "ran, all rows checkable" from "never ran" (invariant #4); one line can report
    // both only if the denominators come first.  The body is in network/qc/Mdb.cpp,
    // next to the aggregation that produces the numbers.
    if (ctx.knobs.mdb_on)
        networkMdbRunEnd(ctx, config_, log);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

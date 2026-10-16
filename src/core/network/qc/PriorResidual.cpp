/**
 * @file PriorResidual.cpp
 * @brief Emission and run-end readout of the prior-residual census
 *        (docs/SPEC_prior_residual_census.md).
 *
 * The statistic lives entirely in the header-only accumulator
 * (@ref NetworkPriorResidual) so it can be unit-tested with no run coupling;
 * this translation unit is the only place that knows about traces, events,
 * counters and the station roster.  It makes no decisions: every number here is
 * a read of what the accumulator already formed, and nothing in the solve, the
 * QC tiers or the emit path reads any of it back.
 */

#include "network/qc/PriorResidual.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

// The run-end reports at the end of this file:
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunGroups.hpp"
#include <memory>
#include "network/driver/Run.hpp"
#include "network/slip/RawObservables.hpp"
#include <map>
#include <string_view>

namespace GnssProcessor {

namespace {

/// Station index -> roster name, or a stable placeholder ("#idx") for an index
/// past the roster.  A shared (satellite-level) prior carries index -1 and
/// reports an empty station, because the row has no station.
std::string stationName(const std::vector<NetStation>& stations, int idx) {
    if (idx < 0) return std::string();
    if (static_cast<std::size_t>(idx) >= stations.size())
        return fmt::format("#{}", idx);
    return stations[static_cast<std::size_t>(idx)].name;
}

/// One CSV field: a finite double, or blank.  Blank is "not measurable", which
/// this file distinguishes from 0.0 everywhere (the "unavailable is not 0"
/// convention the epoch-status writer follows).
std::string num(double v) {
    return std::isfinite(v) ? fmt::format("{:.6e}", v) : std::string();
}

/// Render a @c NetworkPriorResidual::GlobalFlag bitmask as readable text, so the
/// run-end line never asks a reader to decode an integer.
std::string s0gFlagText(int flags) {
    if (flags == 0) return "none";
    std::string t;
    auto add = [&t](const char* s) { if (!t.empty()) t += '+'; t += s; };
    if (flags & NetworkPriorResidual::kGlobalApprox)     add("APPROX(r=1)");
    if (flags & NetworkPriorResidual::kGlobalNoData)     add("NO_DATA(prior-only)");
    if (flags & NetworkPriorResidual::kGlobalNoPrior)    add("NO_PRIOR(data-only)");
    if (flags & NetworkPriorResidual::kGlobalDegenerate) add("DEGENERATE(not formed)");
    if (flags & NetworkPriorResidual::kGlobalRnegDrop)   add("RNEG_DROP");
    return t;
}

}  // namespace

void networkPriorResidualEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces, const SyncedEpoch& se,
                              const std::vector<NetStation>& stations,
                              NetworkPriorResidual& prires, EpochEvents& events) {
    net::LosTable& los_rows = ctx.entities.los;   // written
    if (!prires.enabled()) return;
    EpochEvents::Status& status_ev = events.status;
    const auto t_begin = std::chrono::steady_clock::now();
    const NetworkConfig& nc = config_.getNetworkConfig();
    auto& q = ctx.counters;
    ++q.prires_emit_passes;
    // Mirrored from the accumulator instead of incremented here: beginEpoch runs
    // in SB_T1A and this seam in SB_STASH_STATUS, so the two counts can differ
    // (an epoch withheld between them advanced the census but never drained it).
    // Keeping both makes that state visible at run end.
    q.prires_epochs = prires.epochs();

    // --- the epoch-status scalars (read before the L1 row is written) --------
    status_ev.prires_ep_tow      = se.time.tow;
    status_ev.ep_prires_rows     = prires.epochRows();
    status_ev.ep_prires_resp     = prires.epochRowsResp();
    status_ev.ep_prires_frame    = prires.epochRowsFrame();
    status_ev.ep_prires_reported = prires.epochReported();
    status_ev.ep_prires_max_t    = prires.epochMaxAbsT();
    q.prires_rows     += prires.epochRows();
    q.prires_reported += prires.epochReported();
    q.prires_hits_dropped += prires.hitsDropped();

    // --- the constraint-aware global variance component (the rider) -----------
    // docs/SPEC_prior_residual_census.md §14.  Drained here and nowhere else,
    // because this is the one point at which both sides are complete: the prior
    // side from the five census feeds and the data side from the post-fit RAIM
    // row walk in SB_RAIM, which precedes SB_STASH_STATUS.  Measure-only: nothing
    // reads sigma0_global, and the A4 raim_withhold_sigma0 guard and every gate
    // and deweight use the robust median σ̂₀ formed in NetworkSolveGlue.
    if (prires.globalOn()) {
        const NetworkPriorResidual::GlobalResult g = prires.epochGlobal();
        status_ev.ep_s0g_formed      = g.formed;
        status_ev.ep_s0g_sigma0      = g.sigma0_global;
        status_ev.ep_s0g_tension_def = g.tension_defined;
        status_ev.ep_s0g_tension     = g.prior_tension;
        status_ev.ep_s0g_flags       = g.flags;
        ++q.s0g_epochs;
        if (g.formed) ++q.s0g_formed;
        q.s0g_data_rows  += g.s.n_data;
        q.s0g_prior_rows += g.s.n_prior;
        if (g.flags & NetworkPriorResidual::kGlobalApprox)     ++q.s0g_ep_approx;
        if (g.flags & NetworkPriorResidual::kGlobalNoData)     ++q.s0g_ep_no_data;
        if (g.flags & NetworkPriorResidual::kGlobalNoPrior)    ++q.s0g_ep_no_prior;
        if (g.flags & NetworkPriorResidual::kGlobalDegenerate) ++q.s0g_ep_degenerate;
        if (g.tension_defined && g.prior_tension > q.s0g_max_tension) {
            q.s0g_max_tension     = g.prior_tension;
            q.s0g_max_tension_tow = se.time.tow;
        }
        // Invariant #4: a degraded number is announced in the log the first time
        // it degrades, as well as in-band (the APPROX / NO_DATA bits), so an
        // APPROX column is not quoted without its warning.  One-shot; the run-end
        // line carries the epoch counts.
        if ((g.flags & NetworkPriorResidual::kGlobalApprox)
            && !ctx.counters.s0g_approx_warned) {
            ctx.counters.s0g_approx_warned = true;
            log->warn("Network (constraint-aware global σ̂₀): {} of this epoch's {} data row(s) "
                      "entered with r ≡ 1 at TOW {:.1f} — network.raim_redundancy_normalize is "
                      "off, so Σr_data is a ROW COUNT rather than a redundancy, the denominator "
                      "is too large and sigma0_global reads LOW.  Every affected epoch carries "
                      "the APPROX bit in s0g_flags.  One-shot warning; the run-end line carries "
                      "the epoch total.",
                      g.s.n_data_approx, g.s.n_data, se.time.tow);
        }
        if ((g.flags & NetworkPriorResidual::kGlobalNoData)
            && !ctx.counters.s0g_nodata_warned) {
            ctx.counters.s0g_nodata_warned = true;
            log->warn("Network (constraint-aware global σ̂₀): ZERO data rows at TOW {:.1f} — the "
                      "post-fit RAIM walk contributed nothing (network.raim off, not yet warm, or "
                      "no station qualified), so this epoch's sigma0_global is a PRIOR-ONLY "
                      "variance component and NOT LASER 5-74's global statistic.  The NO_DATA bit "
                      "is set in s0g_flags.  One-shot warning.", se.time.tow);
        }
        // Push the per-epoch sample after the counters, so the run-end quantiles
        // and the counters describe the same population.
        prires.recordEpochGlobal();
    }

    // --- the exceedance ladder, one row per (family x label) (INC-1) ---------
    // Only families/labels that saw a row this epoch get a row: an all-zero row
    // per label per family per epoch would triple the file for no information,
    // and the run-end readout is where "this label never happened" is reported.
    if (traces.prior_residual_trace.is_open()) {
        const auto& fams = prires.families();
        for (std::size_t f = 0; f < fams.size(); ++f) {
            const int fi = static_cast<int>(f);
            for (int li = 0; li < kPriorResidualLabels; ++li) {
                const auto lab = static_cast<PriorResidualLabel>(li);
                const NetworkPriorResidual::Ladder& L = prires.epochLadder(fi, lab);
                // The RNEG class travels in the appended columns of the same row
                // (existing readers keep parsing by position), so a (family,
                // label) whose only rows this epoch were r_negative still emits
                // a row; skipping it would drop those rows from the file.
                const NetworkPriorResidual::Ladder& N = prires.rnegEpochLadder(fi, lab);
                if (L.n_rows == 0 && N.n_rows == 0) continue;
                auto& o = traces.prior_residual_trace;
                o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << fams[f] << ',' << priorResidualLabelName(lab) << ','
                  << L.n_rows << ',' << L.n_folded << ',' << L.n_raw_formal << ','
                  << L.n_r_clamped << ',' << L.n_r_negative << ',' << L.n_no_sigma0 << ','
                  << num(prires.sigma0(fi)) << ',' << (prires.sigma0Warm(fi) ? 1 : 0)
                  << ',' << fmt::format("{:.1f}", prires.sigma0Neff(fi)) << ','
                  << num(L.geoT2());
                for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
                    o << ',' << L.bins[static_cast<std::size_t>(k)];
                o << ',' << num(L.max_abs_t)
                  << ',' << stationName(stations, L.max_station)
                  << ',' << L.max_sat << ',' << L.max_signal << ',' << L.max_mech
                  << ',' << num(L.max_e) << ',' << num(L.max_disp)
                  << ',' << num(L.max_fold_var) << ',' << num(L.max_post_var)
                  << ',' << fmt::format("{:.1f}", L.max_gap_s);
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < kPriorResidualCorrobs; ++c)
                        o << ',' << L.contingency[static_cast<std::size_t>(r)]
                                              [static_cast<std::size_t>(c)];
                o << ',' << fmt::format("{:.2f}", L.meanCorrobDomain())
                  << ',' << L.n_corrob_or_stepped << ',' << L.n_corrob_stepped
                  << ',' << fmt::format("{:.4f}", L.max_corrob_frac);
                // --- the RNEG class, appended (see kPriorResidualHeader) ------
                o << ',' << N.n_rows << ',' << N.n_raw_formal << ',' << N.n_no_sigma0
                  << ',' << num(N.max_abs_e)
                  << ',' << stationName(stations, N.max_station)
                  << ',' << N.max_sat << ',' << N.max_signal << ',' << N.max_mech
                  << ',' << num(N.max_abs_t)
                  << ',' << num(N.max_fold_var) << ',' << num(N.max_post_var)
                  << ',' << fmt::format("{:.1f}", N.max_gap_s)
                  << '\n';
            }
        }
    }

    // --- per-row identities: the PRIORRES event tier -------------------------
    // Rate-limited per (family, station, satellite, signal) like the PREFIT
    // events: a persistently stale prior logs its identity once per holdoff
    // window (network.prior_residual_event_holdoff_ep, in census epochs), not
    // every epoch.  The ladder above is never rate-limited: the holdoff thins
    // the identities, never the counts, so the occupancy numbers stay complete
    // while the event log is a sample.
    const long long holdoff = static_cast<long long>(ctx.knobs.prires_event_holdoff);
    const long long now_ep  = prires.epochs();
    for (const auto& h : prires.hits()) {
        const std::string& fam = prires.families()[static_cast<std::size_t>(h.family_idx)];
        const std::string sta  = stationName(stations, h.station);
        // The line of sight's holdoff clock of (family, signal).  The hit's station index
        // is the row's own (-1 for a shared satellite-level prior, whose name is empty), so
        // no name converter is needed and a shared prior keeps its clock.
        const std::optional<LosId> hid = net::losOf(ctx.entities, h.station, h.sat);
        const std::string sub = fam + "\x1f" + h.signal;
        const long long* last =
            hid ? net::get(ctx.entities.los, *hid, &net::LosState::events, sub,
                           &net::LosEventState::prires_event_last)
                : nullptr;
        if (last != nullptr && now_ep - *last < holdoff)
            continue;
        if (hid)
            net::put(los_rows, *hid, &net::LosState::events, sub,
                     &net::LosEventState::prires_event_last) = now_ep;
        traces.emitEvent(
            se.time, "PRIORRES",
            h.raw_formal ? "prior_residual_raw" : "prior_residual",
            h.sat, sta, std::abs(h.t), ctx.knobs.prires_report_sigma,
            /*action=*/"report",
            fmt::format("family={} label={} sig={} mech={} folded={} e={:.6e} "
                        "disp={:.6e} r={:.4e} s={:.4e} sigma0={:.4f} "
                        "fold_var={:.6e} post_var={:.6e} gap_s={:.1f} corrob={}{}{}",
                        fam, priorResidualLabelName(h.label),
                        h.signal.empty() ? "-" : h.signal.c_str(),
                        (h.mech && *h.mech) ? h.mech : "-",
                        h.folded ? 1 : 0, h.e, h.disp, h.r, h.s, h.sigma0,
                        h.fold_var, h.post_var, h.gap_s,
                        priorResidualCorrobName(h.corrob),
                        // `rneg=1` is a separate token from the `r_negative=1`
                        // text: that bit decides the row's ladder class (the row
                        // is in the RNEG class, out of every bin and out of the
                        // worst-|t| line), and a filter on the event stream needs
                        // one greppable field for it instead of a substring of a
                        // conditional pair.  The `r_clamped` / `r_negative` text
                        // keeps its existing form.
                        h.r_clamped ? (h.r_negative ? " r_clamped=1 r_negative=1"
                                                    : " r_clamped=1") : "",
                        h.r_negative ? " rneg=1" : ""));
        ++q.prires_events;
    }
    // Invariant #4: a truncated identity stream must not read as a quiet epoch.
    if (prires.hitsDropped() > 0 && !ctx.counters.prires_trunc_warned) {
        ctx.counters.prires_trunc_warned = true;
        log->warn("Network (prior-residual census): {} above-threshold row(s) exceeded the "
                  "per-epoch identity buffer ({}) at TOW {:.1f} and their INDIVIDUAL "
                  "identities were dropped.  Every LADDER count is complete and unaffected — "
                  "only the event stream is a sample from here on.  One-shot warning; the "
                  "run-end line carries the total.",
                  prires.hitsDropped(), NetworkPriorResidual::kMaxHitsPerEpoch, se.time.tow);
    }

    // --- worst-of-run identity (run-end line) --------------------------------
    // Two headlines, two classes.  The label ladders are ordered by |t| (the
    // statistic); the RNEG class by |ê| in metres, because its |t| is an
    // artifact of a dispersion that does not describe one fold.
    q.prires_rneg_rows += prires.epochRowsRneg();
    for (std::size_t f = 0; f < prires.families().size(); ++f)
        for (int li = 0; li < kPriorResidualLabels; ++li) {
            const auto lab = static_cast<PriorResidualLabel>(li);
            const auto& N = prires.rnegRunLadder(static_cast<int>(f), lab);
            if (N.max_abs_e > q.prires_rneg_max_e) {
                q.prires_rneg_max_e = N.max_abs_e;
                q.prires_rneg_max_id = fmt::format(
                    "{}/{} {}/{}/{} @{:.1f}", prires.families()[f],
                    priorResidualLabelName(lab),
                    stationName(stations, N.max_station).empty()
                        ? std::string("-") : stationName(stations, N.max_station),
                    N.max_sat.empty() ? std::string("-") : N.max_sat,
                    N.max_signal.empty() ? std::string("-") : N.max_signal,
                    N.max_tow);
            }
            const auto& R = prires.runLadder(static_cast<int>(f), lab);
            if (R.max_abs_t > q.prires_max_t) {
                q.prires_max_t = R.max_abs_t;
                q.prires_max_id = fmt::format(
                    "{}/{} {}/{}/{} @{:.1f}", prires.families()[f],
                    priorResidualLabelName(lab),
                    stationName(stations, R.max_station).empty()
                        ? std::string("-") : stationName(stations, R.max_station),
                    R.max_sat.empty() ? std::string("-") : R.max_sat,
                    R.max_signal.empty() ? std::string("-") : R.max_signal,
                    R.max_tow);
            }
        }

    // Durability (spec §6): only health_trace flushes mid-run.  Flush the ladder
    // when the content hour rolls over, so an aborted long run keeps whole hours
    // of the ladder instead of whatever was in the stream buffer.  At most one
    // flush per hour of content.
    if (traces.prior_residual_trace.is_open()) {
        const int hour = static_cast<int>(se.time.tow / 3600.0);
        if (hour != ctx.carry.prires_last_flush_hour) {
            ctx.carry.prires_last_flush_hour = hour;
            traces.prior_residual_trace.flush();
        }
    }
    (void)nc;
    q.prires_wall_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t_begin).count();
}

void networkPriorResidualRunEnd(const NetworkRunContext& ctx,
                                const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations,
                                const NetworkPriorResidual& prires) {
    if (!prires.enabled()) return;
    const NetworkConfig& nc = config_.getNetworkConfig();
    const auto& q = ctx.counters;
    const auto& lad = NetworkPriorResidual::ladder();
    // Read the epoch denominator from the accumulator, not the counter: the
    // counter is stamped by the emission seam, so a run whose every epoch was
    // withheld between SB_T1A and SB_STASH_STATUS would report 0 there and look
    // like dead wiring when the census advanced but was never drained.
    const long long n_ep = prires.epochs();
    if (n_ep != q.prires_emit_passes)
        log->warn("Network (prior-residual census): {} epoch(s) advanced but only {} were "
                  "DRAINED by the emission seam — the difference is epochs that fed rows "
                  "nobody wrote out (withheld between SB_T1A and SB_STASH_STATUS).  Their "
                  "rows ARE in the run-cumulative ladders below; they have no per-epoch "
                  "trace row and no events.",
                  n_ep, q.prires_emit_passes);

    // Standing invariant #4: the denominators are logged first and
    // unconditionally, because "0 rows over 4-sigma" is a finding only next to
    // the number of epochs run and rows examined.
    log->info("Network (prior-residual census): {} epoch(s) censused ({} emission pass(es)), "
              "{} carried prior(s) examined, {} above the {:g}σ̂₀ TRACE threshold, {} PRIORRES "
              "event(s) emitted after the {}-epoch per-cell holdoff ({} identity(ies) dropped "
              "to the per-epoch buffer cap); {:.1f} ms total in the emission seam.  "
              "MEASURE-ONLY: nothing in the solve read any of this.",
              n_ep, q.prires_emit_passes, q.prires_rows, q.prires_reported,
              nc.prior_residual_report_sigma, q.prires_events,
              std::max(0, nc.prior_residual_event_holdoff_ep), q.prires_hits_dropped,
              q.prires_wall_ms);

    // --- per (family x label) ladders ---------------------------------------
    const auto& fams = prires.families();
    for (std::size_t f = 0; f < fams.size(); ++f) {
        const int fi = static_cast<int>(f);
        const long long fam_rows = prires.runRows(fi);
        if (fam_rows == 0) {
            // Standing invariant #4: separate "never ran" (zero epochs) from
            // "ran and saw nothing", and name which case applies.
            if (n_ep == 0)
                log->warn("Network (prior-residual census): family '{}' ARMED but the census "
                          "NEVER RAN — zero epochs advanced.  This is dead wiring, not a "
                          "negative result; nothing in this run is evidence about the prior-residual census.",
                          fams[f]);
            else
                log->warn("Network (prior-residual census): family '{}' RAN over {} epoch(s) "
                          "and saw ZERO rows.  Either the carry it censuses is off in this "
                          "config, or its feed hook never qualified a row — check the carry's "
                          "own arming knob before reading this as a property of the data.",
                          fams[f], n_ep);
            continue;
        }
        log->info("Network (prior-residual census) family '{}': {} row(s) over {} epoch(s); "
                  "σ̂₀ = {:.4f} ({}, n_eff = {:.0f} of {} calibration rows, warm-up {}).",
                  fams[f], fam_rows, n_ep, prires.sigma0(fi),
                  prires.sigma0Warm(fi) ? "CALIBRATED"
                                        : "WARM-UP ⇒ every t is the RAW FORMAL s",
                  prires.sigma0Neff(fi), prires.sigma0Rows(fi), prires.minRows());
        for (int li = 0; li < kPriorResidualLabels; ++li) {
            const auto lab = static_cast<PriorResidualLabel>(li);
            const auto& R = prires.runLadder(fi, lab);
            // The RNEG class (post_var > fold_var) is reported for every label,
            // including one whose ordinary ladder is empty: a (family, label)
            // whose rows were all r_negative did occur, and reporting "never
            // occurred" would hide the population the RNEG class exists to
            // expose.  It is ranked on |ê| (metres) because its |t| comes from a
            // dispersion that does not describe one fold (the σ̂₀_global rider
            // drops these rows for the same reason, RNEG_DROP).
            const auto& N = prires.rnegRunLadder(fi, lab);
            if (N.n_rows > 0)
                log->info("Network (prior-residual census) '{}'/{} RNEG: n = {} row(s) with "
                          "post_var > fold_var (r < 0 — the coupling-carry case: the harvested "
                          "posterior is a CONDITIONAL variance that can legitimately exceed the "
                          "prior).  Worst |ê| = {:.6e} m at station {} sat '{}' sig '{}' mech "
                          "'{}' @TOW {:.1f} (fold_var = {:.6e}, post_var = {:.6e}, gap = "
                          "{:.1f} s; its |t| would have read {:.4e}, which is why these rows "
                          "are NOT in the ladder, in sum_log_t2, or in the worst-|t| line).",
                          fams[f], priorResidualLabelName(lab), N.n_rows, N.max_abs_e,
                          N.max_station < 0
                              ? std::string("(shared)")
                              : (stationName(stations, N.max_station).empty()
                                     ? fmt::format("#{}", N.max_station)
                                     : stationName(stations, N.max_station)),
                          N.max_sat.empty() ? "-" : N.max_sat,
                          N.max_signal.empty() ? "-" : N.max_signal,
                          (N.max_mech && *N.max_mech) ? N.max_mech : "-", N.max_tow,
                          N.max_fold_var, N.max_post_var, N.max_gap_s, N.max_abs_t);
            if (R.n_rows == 0) {
                log->info("Network (prior-residual census) '{}'/{}: 0 rows{} — this label never "
                          "occurred on this family (ran, saw nothing).",
                          fams[f], priorResidualLabelName(lab),
                          N.n_rows > 0
                              ? fmt::format(" outside the RNEG class ({} r-negative row(s) "
                                            "above)", N.n_rows)
                              : std::string());
                continue;
            }
            std::string ladder_txt;
            for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
                ladder_txt += fmt::format("{}>{:g}:{} ({:.3f}%)",
                                          k ? "  " : "", lad[static_cast<std::size_t>(k)],
                                          R.bins[static_cast<std::size_t>(k)],
                                          100.0 * R.frac(k));
            log->info("Network (prior-residual census) '{}'/{}: n = {} ({} folded, {} raw-formal, "
                      "{} r-floored, {} withheld from σ̂₀; {} further row(s) were r-NEGATIVE and "
                      "sit in the RNEG class below, NOT in these bins), geo(t²) = {:.4f}.  "
                      "LADDER  {}",
                      fams[f], priorResidualLabelName(lab), R.n_rows, R.n_folded,
                      R.n_raw_formal, R.n_r_clamped, R.n_no_sigma0,
                      prires.rnegRunLadder(fi, lab).n_rows,
                      R.geoT2(), ladder_txt);
            if (R.max_abs_t > 0.0)
                log->info("Network (prior-residual census) '{}'/{}: worst |t| = {:.4e} at "
                          "station {} sat '{}' sig '{}' mech '{}' @TOW {:.1f} "
                          "(ê = {:.6e} m, D{{ê}} = {:.6e} m², fold_var = {:.6e}, "
                          "post_var = {:.6e}, gap = {:.1f} s).",
                          fams[f], priorResidualLabelName(lab), R.max_abs_t,
                          // -1 is a shared (satellite-level) prior: the row has
                          // no station, which is not a lookup failure, so it
                          // prints "(shared)" instead of "#-1".
                          R.max_station < 0
                              ? std::string("(shared)")
                              : (stationName(stations, R.max_station).empty()
                                     ? fmt::format("#{}", R.max_station)
                                     : stationName(stations, R.max_station)),
                          R.max_sat.empty() ? "-" : R.max_sat,
                          R.max_signal.empty() ? "-" : R.max_signal,
                          (R.max_mech && *R.max_mech) ? R.max_mech : "-", R.max_tow,
                          R.max_e, R.max_disp, R.max_fold_var, R.max_post_var,
                          R.max_gap_s);
        }
        // Positive control (spec §9.3): the ratio of 4σ̂₀ ladder occupancy,
        // Responded : Clean.  It is meaningful only on the unfolded `+` branch;
        // under the folded form a refloated row's r hits the ε clamp and the
        // shift is an artifact of the clamp, not a measurement.  A null result is
        // a finding, not a failed gate: the prior-residual statistic has no power
        // on this family.
        const auto& Rr = prires.runLadder(fi, PriorResidualLabel::Responded);
        const auto& Rc = prires.runLadder(fi, PriorResidualLabel::Clean);
        if (Rr.n_rows > 0 && Rc.n_rows > 0) {
            const double fr = Rr.frac(NetworkPriorResidual::kLadder4Sigma);
            const double fc = Rc.frac(NetworkPriorResidual::kLadder4Sigma);
            const double ratio = (fc > 0.0) ? fr / fc
                                            : std::numeric_limits<double>::infinity();
            log->info("Network (prior-residual census) '{}' POSITIVE CONTROL: 4σ̂₀ occupancy "
                      "RESPONDED {:.3f}% ({} of {}; {} folded / {} on the unfolded + branch) vs "
                      "CLEAN {:.3f}% ({} of {}; {} folded / {} unfolded) — ratio {:.2f}.  A ratio "
                      "near 1 is a NULL RESULT and a publishable finding, not a failed gate: it "
                      "says this statistic has no power on this family, which is the same class "
                      "of result as D5's 2.1 %.",
                      fams[f], 100.0 * fr, Rr.bins[NetworkPriorResidual::kLadder4Sigma],
                      Rr.n_rows, Rr.n_folded, Rr.n_rows - Rr.n_folded, 100.0 * fc,
                      Rc.bins[NetworkPriorResidual::kLadder4Sigma], Rc.n_rows, Rc.n_folded,
                      Rc.n_rows - Rc.n_folded, ratio);
            // The comparison is a measurement only when both sides sit on the same
            // dispersion branch.  An all-unfolded Responded population against an
            // all-folded Clean population compares two different nulls, and the
            // ratio then reflects the branch, not power; the warning says so.
            const bool rr_unfolded = Rr.n_folded == 0;
            const bool rc_folded   = Rc.n_folded == Rc.n_rows;
            if (rr_unfolded && rc_folded)
                log->warn("Network (prior-residual census) '{}': the two populations above sit "
                          "on DIFFERENT dispersion branches — every RESPONDED row is unfolded "
                          "(D = fold_var + post_var) and every CLEAN row is folded "
                          "(D = fold_var - post_var).  That is structurally correct (a responded "
                          "prior did not enter the solve) but it means the ratio compares two "
                          "different nulls and is NOT a power measurement.  Spec §9.3 is gradable "
                          "only over rows on the SAME branch; report the two ladders side by side "
                          "instead of their ratio.", fams[f]);
        } else if (Rr.n_rows == 0) {
            log->info("Network (prior-residual census) '{}': NO RESPONDED rows all run — the "
                      "positive control had ZERO opportunity, which is NOT evidence that the "
                      "statistic has power (nor that it lacks it).  A run with no slip "
                      "refloats, no reseeds and no exclusions on this family cannot grade "
                      "spec §9.3.", fams[f]);
        }
        // --- contingency table (spec §4/§9.6) (INC-3) ------------------------
        if (prires.corroborating()) {
            for (int li = 0; li < kPriorResidualLabels; ++li) {
                const auto lab = static_cast<PriorResidualLabel>(li);
                const auto& R = prires.runLadder(fi, lab);
                long long tot = 0;
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < kPriorResidualCorrobs; ++c)
                        tot += R.contingency[static_cast<std::size_t>(r)]
                                            [static_cast<std::size_t>(c)];
                if (tot == 0) continue;
                const long long a = R.contingency[0][2], b = R.contingency[0][1],
                                c0 = R.contingency[0][0];
                const long long d = R.contingency[1][2], e = R.contingency[1][1],
                                f0 = R.contingency[1][0];
                const long long hi = a + b + c0;
                log->info("Network (prior-residual census) '{}'/{} CONTINGENCY (rows above the "
                          "{:g}σ̂₀ threshold vs the raw-observable verdict): "
                          "raw_stepped {} | raw_flat {} | no_evidence {}  ||  at-or-below: "
                          "raw_stepped {} | raw_flat {} | no_evidence {}.  THE B-CELL is "
                          "raw_flat-above = {} of {} ({:.1f}%): flagged rows with POSITIVE "
                          "evidence that the raw observable did NOT move.  On the DD family "
                          "this measured 97 %, which is what revealed D5's population to be a "
                          "persistent model error rather than slips.",
                          fams[f], priorResidualLabelName(lab),
                          nc.prior_residual_report_sigma, a, b, c0, d, e, f0, b, hi,
                          (hi > 0) ? 100.0 * static_cast<double>(b)
                                             / static_cast<double>(hi) : 0.0);
                // Corroboration audit for the b-cell above.  The consensus rule
                // can only move rows Stepped -> Flat, so it can only raise the
                // b-cell; the audit shows how many rows it moved and over what
                // domain size.  A mean domain of 1.00 means the rule was a no-op
                // for this family (`pl`); a mean domain near the station count is
                // the regime where the v1 OR rule saturates and the b-cell stops
                // being a rate.
                log->info("Network (prior-residual census) '{}'/{} CORROBORATION AUDIT: mean "
                          "domain {:.2f} (station,sat) pair(s) per row over {} row(s) carrying "
                          "evidence; v1's saturating OR would have called {} row(s) Stepped, "
                          "the CONSENSUS called {} ({:+.1f}% of rows moved Stepped→Flat).  A "
                          "mean domain of 1.00 means the rule was a NO-OP here; a mean domain "
                          "near the roster size is exactly where the OR stopped discriminating "
                          "(measured: cb raw_stepped 29 % at 63 sta → 70 % at 305 under the OR, "
                          "b-cell 57 % → 21 %, on identical data).",
                          fams[f], priorResidualLabelName(lab), R.meanCorrobDomain(),
                          R.n_corrob_rows, R.n_corrob_or_stepped, R.n_corrob_stepped,
                          (R.n_corrob_rows > 0)
                              ? -100.0 * static_cast<double>(R.n_corrob_or_stepped
                                                             - R.n_corrob_stepped)
                                       / static_cast<double>(R.n_corrob_rows)
                              : 0.0);
                // Consensus sensitivity.  "0 Stepped" reads the same whether no
                // common-mode event occurred or the rule could not have seen one.
                // The bar is a strict majority (0.5000); the highest stepped
                // fraction any domain reached separates the two readings.
                if (R.n_corrob_rows > 0)
                    log->info("Network (prior-residual census) '{}'/{} CONSENSUS SENSITIVITY: "
                              "the highest stepped FRACTION any domain reached all run was "
                              "{:.4f}, against the rule's strict-majority bar of 0.5000.  {}",
                              fams[f], priorResidualLabelName(lab), R.max_corrob_frac,
                              (R.max_corrob_frac >= 0.5)
                                  ? "The bar sits INSIDE the observed distribution, so the "
                                    "consensus count above means what it appears to mean."
                                  : (R.max_corrob_frac >= 0.25)
                                      ? "The bar was approached but not reached; a moderate "
                                        "common-mode event would plausibly have been caught."
                                      : "The bar was NEVER APPROACHED — it is out of range of "
                                        "anything this window produced, so a LOW CONSENSUS "
                                        "COUNT IS NOT EVIDENCE that the rule would catch a real "
                                        "satellite-side or receiver-side discontinuity.  Grade "
                                        "the rule's power on a window with a KNOWN event before "
                                        "trusting a null here.");
            }
        }
    }
    if (q.prires_max_t > 0.0)
        log->info("Network (prior-residual census): worst |t| of the run = {:.4e} at {} "
                  "(RNEG rows EXCLUDED — see the next line).",
                  q.prires_max_t, q.prires_max_id);
    // The RNEG headline is in metres and sits beside the |t| line: RNEG rows
    // have inflated |t| and would otherwise dominate the worst-|t| headline, so
    // they are reported here, ranked on |ê|, instead.
    if (q.prires_rneg_rows > 0)
        log->info("Network (prior-residual census) RNEG: {} row(s) of the run had post_var > "
                  "fold_var (r < 0) and are excluded from every ladder bin, from sum_log_t2 "
                  "and from the worst-|t| line above — their dispersion comes from a (prior, "
                  "posterior) pair that does not describe ONE fold, so r hits the redundancy "
                  "floor and |t| inflates by √((p+v)/(εp)).  Worst |ê| = {:.6e} m at {}.  "
                  "(The constraint-aware σ̂₀_global rider drops the same rows — flag "
                  "RNEG_DROP.)",
                  q.prires_rneg_rows, q.prires_rneg_max_e,
                  q.prires_rneg_max_id.empty() ? std::string("-") : q.prires_rneg_max_id);

    // --- the constraint-aware global variance component -----------------------
    // docs/SPEC_prior_residual_census.md §14.2/§14.6.  Invariant #4: the
    // denominators and the degradation mode are logged before any number,
    // because sigma0_global means nothing without knowing whether its data
    // side was r ≡ 1 or empty.
    if (prires.globalOn()) {
        const NetworkPriorResidual::GlobalResult G = prires.runGlobal();
        const auto& S = G.s;
        log->info("Network (constraint-aware global σ̂₀): {} drained epoch(s) evaluated, {} with a "
                  "formed σ̂₀²_global; {} data row(s) on the RAIM side, {} folded prior row(s) on "
                  "the census side.  Degradation: {} epoch(s) APPROX (r ≡ 1 data side), {} with "
                  "NO data rows (prior-only), {} with NO folded prior rows (data-only), {} "
                  "DEGENERATE (Σr ≤ 0, no number formed).  Per-epoch history: {} sample(s) "
                  "retained{}.",
                  q.s0g_epochs, q.s0g_formed, q.s0g_data_rows, q.s0g_prior_rows,
                  q.s0g_ep_approx, q.s0g_ep_no_data, q.s0g_ep_no_prior, q.s0g_ep_degenerate,
                  prires.globalSampleCount(),
                  prires.globalSamplesDropped() > 0
                      ? fmt::format(", {} DROPPED past the {}-sample cap (the quantiles below "
                                    "cover only the retained prefix)",
                                    prires.globalSamplesDropped(),
                                    NetworkPriorResidual::kMaxGlobalSamples)
                      : std::string());
        if (q.s0g_epochs == 0) {
            log->warn("Network (constraint-aware global σ̂₀): ARMED but NEVER DRAINED — zero "
                      "epochs reached the emission seam.  This is dead wiring, not a negative "
                      "result; nothing in this run is evidence about the global σ̂₀.");
        } else if (!G.formed) {
            log->warn("Network (constraint-aware global σ̂₀): the run-pooled Σr is {:.6e} over "
                      "{} data + {} prior row(s) — no σ̂₀²_global was formed for the RUN.  Flags: "
                      "{}.  Check the two feed sites before reading anything else here.",
                      S.r_data + S.r_prior, S.n_data, S.n_prior, s0gFlagText(G.flags));
        } else {
            // The run-pooled estimator (Σ over every row of the run) is the
            // variance component; the per-epoch median/p95 that follow are a
            // different statistic, and the log line says so.
            // An undefined tension is written as a word, not as nan: it is a
            // structural property of the run (no data side, so no denominator),
            // not a numerical accident.
            log->info("Network (constraint-aware global σ̂₀) RUN-POOLED: "
                      "σ̂₀²_global = (Σw·v² + Σw·ê²)/(Σr_data + Σr_prior) = "
                      "({:.6e} + {:.6e})/({:.6e} + {:.6e}) = {:.6e}  ⇒  σ̂₀_global = {:.4f}.  "
                      "PRIOR TENSION Σw·ê²/Σw·v² = {} — the fraction of the run's weighted "
                      "misfit that lives in the CARRY rather than in the data, which the robust "
                      "median σ̂₀ is structurally blind to.  Flags: {}.",
                      S.wv2, S.we2, S.r_data, S.r_prior, G.sigma0_global_v, G.sigma0_global,
                      G.tension_defined
                          ? fmt::format("{:.4e}", G.prior_tension)
                          : std::string("UNDEFINED (no data side — there is nothing to divide "
                                        "by; the numerator is the carry's alone)"),
                      s0gFlagText(G.flags));
            if (prires.tensionSampleCount() > 0)
                log->info("Network (constraint-aware global σ̂₀) PER-EPOCH DISTRIBUTION (a "
                          "different statistic from the pooled one above — a median of ratios, "
                          "not a ratio of sums): σ̂₀_global p05 {:.4f} / median {:.4f} / p95 "
                          "{:.4f} over {} epoch(s); prior tension p05 {:.4e} / median {:.4e} / "
                          "p95 {:.4e} over {} epoch(s).  Worst epoch tension {:.4e} @TOW {:.1f}.",
                          prires.globalQuantile(0.05), prires.globalQuantile(0.50),
                          prires.globalQuantile(0.95), prires.globalSampleCount(),
                          prires.tensionQuantile(0.05), prires.tensionQuantile(0.50),
                          prires.tensionQuantile(0.95), prires.tensionSampleCount(),
                          q.s0g_max_tension, q.s0g_max_tension_tow);
            else
                log->warn("Network (constraint-aware global σ̂₀) PER-EPOCH DISTRIBUTION: "
                          "σ̂₀_global p05 {:.4f} / median {:.4f} / p95 {:.4f} over {} epoch(s); "
                          "PRIOR TENSION has NO SAMPLES — not one epoch had a data side, so the "
                          "estimate's HEADLINE is unmeasured on this run.  That is a property of the "
                          "LINEAGE, not of the data: the post-fit RAIM pass the data side rides "
                          "is bracketed by `raim_on && carry_warm`, and carry_warm requires "
                          "network.phase_leveling.  Grade this estimate on a phase_leveling lineage.",
                          prires.globalQuantile(0.05), prires.globalQuantile(0.50),
                          prires.globalQuantile(0.95), prires.globalSampleCount());
        }
        // Per-family split (spec §6): which carry family the prior-side tension
        // came from.  Each family's variance factor is expected to agree with the
        // M2 per-family robust σ̂ to about a factor of 2; a larger disagreement
        // points to a defect in one of the three instruments.
        if (S.we2 > 0.0 || S.n_prior > 0 || S.n_prior_unfolded > 0
            || S.n_prior_nominal > 0 || S.n_prior_rneg > 0) {
            for (std::size_t f = 0; f < fams.size(); ++f) {
                const auto& F = prires.familyGlobalRun(static_cast<int>(f));
                if (F.n_prior == 0 && F.n_prior_unfolded == 0
                    && F.n_prior_nominal == 0 && F.n_prior_rneg == 0)
                    continue;
                log->info("Network (constraint-aware global σ̂₀) family '{}': {} folded row(s) "
                          "contributed Σw·ê² = {:.6e} ({:.2f}% of the prior side, {:.2f}% of the "
                          "whole numerator) and Σr = {:.6e} ({:.2f}% of the whole redundancy); "
                          "mean w·ê² per row = {:.4e} against a mean r of {:.4e} (the two are "
                          "EQUAL in expectation under the model — their ratio IS this family's "
                          "variance factor: {:.4f}).  Skipped: {} unfolded (not a constraint this "
                          "epoch), {} nominal-variance, {} with post_var > fold_var.",
                          fams[f], F.n_prior, F.we2,
                          S.we2 > 0.0 ? 100.0 * F.we2 / S.we2 : 0.0,
                          (S.we2 + S.wv2) > 0.0 ? 100.0 * F.we2 / (S.we2 + S.wv2) : 0.0,
                          F.r_prior,
                          (S.r_data + S.r_prior) > 0.0
                              ? 100.0 * F.r_prior / (S.r_data + S.r_prior) : 0.0,
                          F.n_prior > 0 ? F.we2 / static_cast<double>(F.n_prior) : 0.0,
                          F.n_prior > 0 ? F.r_prior / static_cast<double>(F.n_prior) : 0.0,
                          F.r_prior > 0.0 ? F.we2 / F.r_prior
                                          : std::numeric_limits<double>::quiet_NaN(),
                          F.n_prior_unfolded, F.n_prior_nominal, F.n_prior_rneg);
            }
        }
        log->info("Network (constraint-aware global σ̂₀): ⚠ THE DATA SIDE IS THE POST-FIT RAIM "
                  "POOL, NOT THE WHOLE SYSTEM — that walk visits the PHASE rows of CARRIED "
                  "AMBIGUITIES at non-pivot stations, so Σr_data is NOT n − rk A of the network "
                  "and σ̂₀_global is a variance component over (RAIM phase rows ∪ census prior "
                  "rows).  MEASURE-ONLY: the A4 raim_withhold_sigma0 guard, every gate and every "
                  "deweight ran on the ROBUST MEDIAN σ̂₀ and read NOTHING above.  It is a SCALE, "
                  "not a test statistic — it inherits the same heavy-tail reality the ladders "
                  "measure, and a t/F table on it rebuilds D5 v1.");
    }
    // Closing line: every number above is a measurement, not a defect claim.
    log->info("Network (prior-residual census): every number above is a MEASUREMENT of the "
              "carried stochastic model, not a defect claim, and no gate, deweight, exclusion "
              "or refloat was wired to any of it.  The critical value is EMPIRICAL, never "
              "tabular — a Gaussian t-table on this data rebuilds D5 v1.  The response "
              "question is a SEPARATE change whose design input is these ladders.");
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the prior-residual census.
int networkPriorResidualRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkSetupResult& setup = v.setup;
    const NetworkPriorResidual& prires_est = v.run.engines.prires;

    // --- Prior-residual census run-end telemetry ------------------------------
    // (docs/SPEC_prior_residual_census.md §6).  The ladder is the telemetry
    // (invariant #4), and a family with n_rows = 0 has to read as "ran, saw nothing"
    // rather than "never ran".  The body is in network/qc/PriorResidual.cpp, next
    // to the emission that feeds it.
    if (ctx.knobs.prires_on)
        networkPriorResidualRunEnd(ctx, config_, log, setup.stations, prires_est);
    return 0;
}

// ---- End of the run-end reports ----

// Setup builder; documented at its declaration in network/qc/PriorResidual.hpp.
void networkPriorResidualArm(NetworkRun& run) {
    // ---- From the run (generated by scripts/nom_ep4_cut.py, each line
    //      checked by scripts/nom_ep4_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    const SlipQcChannel& slip_qc = run.engines.slip_qc;
    NetworkPriorResidual& prires_est = run.engines.prires;
    // ---- End of the bindings ----
    // Prior-residual census.  An engine the run owns, like the carry-VCE monitor
    // (run.engines.prires and run.engines.carry_vce, network/driver/Run.hpp; both were
    // run-TU locals formerly), read by the same four seams: SB_T1A
    // advances it, SB_LEVELING feeds pl/zwd/rxcb, SB_STASH_STATUS feeds pb/cb and
    // drains it, and the teardown reads it.  It shares the M2 monitor's per-row
    // arithmetic and does not affect the solve: with the census armed the
    // solution is byte-identical.
    // Corroboration resolver.  It is injected here rather than plumbed through five
    // seam signatures, and it captures slip_qc by reference because the evidence
    // it reads is refreshed per epoch inside that channel.
    //
    // The resolver gathers domain counts only; the verdict rule is @ref
    // priorResidualConsensus, a pure function with its own unit cases.  The split
    // makes the roster-independence claim checkable without a network.  A plain OR
    // over a domain saturates as the roster grows, because P(any of N steps) -> 1;
    // on a 303-station roster it moved `cb` rows reading raw_stepped from 29 % to
    // 70 % on identical data.
    //
    // A carried prior's domain is the set of (station, satellite) pairs whose raw
    // MW/GF observables can witness a discontinuity in it:
    //   * `pl`          -> one pair (station, sat): the arc's own verdict;
    //   * `zwd`, `rxcb` -> receiver level: that station's satellites;
    //   * `pb`, `cb`    -> satellite level: the stations that measured that sat.
    //
    // Both multi-member domains are aggregated once per epoch into the maps below
    // rather than re-scanned per row.  Otherwise a shared-family row would walk
    // every station map (131 cb rows x 303 stations = 40k lookups per epoch),
    // making the census quadratic in the roster.  Building the maps is one pass
    // over the verdict table, and the per-row lookup is then O(log n).
    //
    // The cache is keyed on slip_qc.epoch_idx, the monotonic processed-epoch
    // index, so it invalidates itself exactly when the evidence is refreshed.  All
    // five feed sites run serially after the solve, so the mutable capture is
    // race-free by construction (the parallel produce phase never reaches it).
    if (ctx.knobs.prires_corrob) {
        struct CorrobCache {
            long long epoch = -1;
            std::map<std::string, PriorResidualEvidence> by_sat;   // satellite-level domains
            std::vector<PriorResidualEvidence>           by_sta;   // receiver-level domains
        };
        auto cache = std::make_shared<CorrobCache>();
        prires_est.setCorroborator(
            [&slip_qc, cache](std::string_view /*family*/, int station,
                              std::string_view sat) -> PriorResidualEvidence {
                const RawSlipEpoch& ep = slip_qc.epoch;
                if (!ep.computed) return PriorResidualEvidence{};
                if (cache->epoch != slip_qc.epoch_idx) {
                    cache->epoch = slip_qc.epoch_idx;
                    cache->by_sat.clear();
                    cache->by_sta.assign(ep.verdict.size(), PriorResidualEvidence{});
                    for (std::size_t i = 0; i < ep.verdict.size(); ++i)
                        for (const auto& kv : ep.verdict[i]) {
                            if (!kv.second.measured) continue;   // no predecessor ⇒ in no domain
                            const int step = kv.second.stepped ? 1 : 0;
                            PriorResidualEvidence& a = cache->by_sta[i];
                            ++a.n_measured; a.n_stepped += step;
                            PriorResidualEvidence& b = cache->by_sat[kv.first];
                            ++b.n_measured; b.n_stepped += step;
                        }
                }
                if (station >= 0 && static_cast<std::size_t>(station) < ep.verdict.size()) {
                    if (sat.empty()) return cache->by_sta[static_cast<std::size_t>(station)];
                    // pl has a one-member domain, where priorResidualConsensus
                    // reduces to the member's own verdict, so this family is
                    // bit-for-bit unaffected by the consensus rule.
                    const auto& vmap = ep.verdict[static_cast<std::size_t>(station)];
                    const auto it = vmap.find(std::string(sat));
                    if (it == vmap.end() || !it->second.measured)
                        return PriorResidualEvidence{};
                    return PriorResidualEvidence{1, it->second.stepped ? 1 : 0};
                }
                if (!sat.empty()) {
                    const auto it = cache->by_sat.find(std::string(sat));
                    if (it != cache->by_sat.end()) return it->second;
                }
                return PriorResidualEvidence{};
            });
    }
}

}  // namespace GnssProcessor

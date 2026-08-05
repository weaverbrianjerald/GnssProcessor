/**
 * @file NetworkPriorResidual.cpp
 * @brief Emission + run-end readout of the PRIOR-RESIDUAL CENSUS
 *        (docs/SPEC_prior_residual_census.md).
 *
 * The statistic itself lives entirely in the header-only accumulator
 * (@ref NetworkPriorResidual) so it can be unit-tested with no run coupling;
 * this translation unit is the only place that knows about traces, events,
 * counters and the station roster.  It DECIDES NOTHING — every number here is a
 * read of what the accumulator already formed, and nothing in the solve, the QC
 * tiers or the emit path reads any of it back.
 */

#include "filter/NetworkPriorResidual.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace GnssProcessor {

namespace {

/// Station index -> roster name, or a stable placeholder.  A shared
/// (satellite-level) prior carries index -1 and reports an EMPTY station, which
/// is the honest statement: the row has no station.
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
                              NetworkPriorResidual& prires) {
    if (!prires.enabled()) return;
    const auto t_begin = std::chrono::steady_clock::now();
    const NetworkConfig& nc = config_.getNetworkConfig();
    auto& q = ctx.counters;
    ++q.prires_emit_passes;
    // Mirrored from the accumulator rather than incremented here: beginEpoch runs
    // in SB_T1A and this seam in SB_STASH_STATUS, so the two can legitimately
    // differ (an epoch withheld between them advanced the census but never
    // drained it).  Keeping both visible is what makes that state readable at run
    // end instead of silently absorbed.
    q.prires_epochs = prires.epochs();

    // --- the epoch-status scalars (read BEFORE the L1 row is written) --------
    ctx.carry.prires_ep_tow      = se.time.tow;
    ctx.carry.ep_prires_rows     = prires.epochRows();
    ctx.carry.ep_prires_resp     = prires.epochRowsResp();
    ctx.carry.ep_prires_frame    = prires.epochRowsFrame();
    ctx.carry.ep_prires_reported = prires.epochReported();
    ctx.carry.ep_prires_max_t    = prires.epochMaxAbsT();
    q.prires_rows     += prires.epochRows();
    q.prires_reported += prires.epochReported();
    q.prires_hits_dropped += prires.hitsDropped();

    // --- the constraint-aware global variance component (the rider) -----------
    // docs/SPEC_sigma0_constraint_aware.md.  Drained HERE and only here: this is
    // the one point at which both sides are complete — the prior side from the
    // five census feeds (the last of them a few lines above) and the data side
    // from the post-fit RAIM row walk in SB_RAIM, which precedes SB_STASH_STATUS.
    // MEASURE-ONLY, one level deeper than the census itself: nothing reads
    // sigma0_global, and the A4 raim_withhold_sigma0 guard plus every gate and
    // deweight stay on the ROBUST MEDIAN σ̂₀ formed in NetworkSolveGlue.
    if (prires.globalOn()) {
        const NetworkPriorResidual::GlobalResult g = prires.epochGlobal();
        ctx.carry.ep_s0g_formed      = g.formed;
        ctx.carry.ep_s0g_sigma0      = g.sigma0_global;
        ctx.carry.ep_s0g_tension_def = g.tension_defined;
        ctx.carry.ep_s0g_tension     = g.prior_tension;
        ctx.carry.ep_s0g_flags       = g.flags;
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
        // Invariant #4: a DEGRADED number says so the first time it degrades, in
        // the log as well as in-band, so nobody quotes an APPROX column from a
        // run whose warning they never saw.  One-shot; the run-end line carries
        // the epoch counts.
        if ((g.flags & NetworkPriorResidual::kGlobalApprox)
            && !ctx.carry.s0g_approx_warned) {
            ctx.carry.s0g_approx_warned = true;
            log->warn("Network (constraint-aware global σ̂₀): {} of this epoch's {} data row(s) "
                      "entered with r ≡ 1 at TOW {:.1f} — network.raim_redundancy_normalize is "
                      "off, so Σr_data is a ROW COUNT rather than a redundancy, the denominator "
                      "is too large and sigma0_global reads LOW.  Every affected epoch carries "
                      "the APPROX bit in s0g_flags.  One-shot warning; the run-end line carries "
                      "the epoch total.",
                      g.s.n_data_approx, g.s.n_data, se.time.tow);
        }
        if ((g.flags & NetworkPriorResidual::kGlobalNoData)
            && !ctx.carry.s0g_nodata_warned) {
            ctx.carry.s0g_nodata_warned = true;
            log->warn("Network (constraint-aware global σ̂₀): ZERO data rows at TOW {:.1f} — the "
                      "post-fit RAIM walk contributed nothing (network.raim off, not yet warm, or "
                      "no station qualified), so this epoch's sigma0_global is a PRIOR-ONLY "
                      "variance component and NOT LASER 5-74's global statistic.  The NO_DATA bit "
                      "is set in s0g_flags.  One-shot warning.", se.time.tow);
        }
        // Push the per-epoch sample AFTER the counters, so the run-end quantiles
        // and the counters describe the same population.
        prires.recordEpochGlobal();
    }

    // --- INC-1: the exceedance ladder, one row per (family x label) ----------
    // Only families/labels that saw a row this epoch get a row: an all-zero row
    // per label per family per epoch would triple the file for no information,
    // and the RUN-end readout is where "this label never happened" is reported.
    if (traces.prior_residual_trace.is_open()) {
        const auto& fams = prires.families();
        for (std::size_t f = 0; f < fams.size(); ++f) {
            const int fi = static_cast<int>(f);
            for (int li = 0; li < kPriorResidualLabels; ++li) {
                const auto lab = static_cast<PriorResidualLabel>(li);
                const NetworkPriorResidual::Ladder& L = prires.epochLadder(fi, lab);
                if (L.n_rows == 0) continue;
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
                  << ',' << fmt::format("{:.4f}", L.max_corrob_frac)
                  << '\n';
            }
        }
    }

    // --- per-row identities: the PRIORRES event tier -------------------------
    // Rate-limited per (family, station, satellite, signal) exactly like the
    // PREFIT events: a prior that is persistently stale should log its identity
    // PERSISTENTLY, not 120x/hour.  The LADDER above is never rate-limited — the
    // holdoff only thins the identities, never the counts, which is the property
    // that lets a reader trust the occupancy numbers against a sampled event log.
    const long long holdoff = static_cast<long long>(ctx.knobs.prires_event_holdoff);
    const long long now_ep  = prires.epochs();
    for (const auto& h : prires.hits()) {
        const std::string& fam = prires.families()[static_cast<std::size_t>(h.family_idx)];
        const std::string sta  = stationName(stations, h.station);
        const std::string key  = fam + "\x1f" + sta + "\x1f" + h.sat + "\x1f" + h.signal;
        auto it = ctx.carry.prires_event_last.find(key);
        if (it != ctx.carry.prires_event_last.end() && now_ep - it->second < holdoff)
            continue;
        ctx.carry.prires_event_last[key] = now_ep;
        traces.emitEvent(
            se.time, "PRIORRES",
            h.raw_formal ? "prior_residual_raw" : "prior_residual",
            h.sat, sta, std::abs(h.t), ctx.knobs.prires_report_sigma,
            /*action=*/"report",
            fmt::format("family={} label={} sig={} mech={} folded={} e={:.6e} "
                        "disp={:.6e} r={:.4e} s={:.4e} sigma0={:.4f} "
                        "fold_var={:.6e} post_var={:.6e} gap_s={:.1f} corrob={}{}",
                        fam, priorResidualLabelName(h.label),
                        h.signal.empty() ? "-" : h.signal.c_str(),
                        (h.mech && *h.mech) ? h.mech : "-",
                        h.folded ? 1 : 0, h.e, h.disp, h.r, h.s, h.sigma0,
                        h.fold_var, h.post_var, h.gap_s,
                        priorResidualCorrobName(h.corrob),
                        h.r_clamped ? (h.r_negative ? " r_clamped=1 r_negative=1"
                                                    : " r_clamped=1") : ""));
        ++q.prires_events;
    }
    // Invariant #4: a truncated identity stream must not read as a quiet epoch.
    if (prires.hitsDropped() > 0 && !ctx.carry.prires_trunc_warned) {
        ctx.carry.prires_trunc_warned = true;
        log->warn("Network (prior-residual census): {} above-threshold row(s) exceeded the "
                  "per-epoch identity buffer ({}) at TOW {:.1f} and their INDIVIDUAL "
                  "identities were dropped.  Every LADDER count is complete and unaffected — "
                  "only the event stream is a sample from here on.  One-shot warning; the "
                  "run-end line carries the total.",
                  prires.hitsDropped(), NetworkPriorResidual::kMaxHitsPerEpoch, se.time.tow);
    }

    // --- worst-of-run identity (run-end line) --------------------------------
    for (std::size_t f = 0; f < prires.families().size(); ++f)
        for (int li = 0; li < kPriorResidualLabels; ++li) {
            const auto lab = static_cast<PriorResidualLabel>(li);
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

    // ⚠ Durability (spec §6): only health_trace flushes mid-run.  Flush the
    // ladder on the content-hour roll so a killed long run keeps whole hours of
    // the card's headline product rather than whatever happened to be in the
    // stream buffer.  Cheap: at most one flush per hour of content.
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

void networkPriorResidualReport(const NetworkRunContext& ctx,
                                const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations,
                                const NetworkPriorResidual& prires) {
    if (!prires.enabled()) return;
    const NetworkConfig& nc = config_.getNetworkConfig();
    const auto& q = ctx.counters;
    const auto& lad = NetworkPriorResidual::ladder();
    // Read the epoch denominator off the ACCUMULATOR, not the counter: the
    // counter is stamped by the emission seam, so a run whose every epoch was
    // withheld between SB_T1A and SB_STASH_STATUS would report 0 there and read
    // as dead wiring when it is in fact "advanced but never drained".
    const long long n_ep = prires.epochs();
    if (n_ep != q.prires_emit_passes)
        log->warn("Network (prior-residual census): {} epoch(s) advanced but only {} were "
                  "DRAINED by the emission seam — the difference is epochs that fed rows "
                  "nobody wrote out (withheld between SB_T1A and SB_STASH_STATUS).  Their "
                  "rows ARE in the run-cumulative ladders below; they have no per-epoch "
                  "trace row and no events.",
                  n_ep, q.prires_emit_passes);

    // Standing invariant #4, ladder edition.  The DENOMINATORS come first and
    // unconditionally: "0 rows over 4-sigma" is a finding only when the reader
    // can see how many epochs ran and how many rows were examined.
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
            // The distinction standing invariant #4 exists for.  q.prires_epochs
            // separates the two cases and the message must NAME which one it is.
            if (n_ep == 0)
                log->warn("Network (prior-residual census): family '{}' ARMED but the census "
                          "NEVER RAN — zero epochs advanced.  This is dead wiring, not a "
                          "negative result; nothing in this run is evidence about the card.",
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
            if (R.n_rows == 0) {
                log->info("Network (prior-residual census) '{}'/{}: 0 rows — this label never "
                          "occurred on this family (ran, saw nothing).",
                          fams[f], priorResidualLabelName(lab));
                continue;
            }
            std::string ladder_txt;
            for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
                ladder_txt += fmt::format("{}>{:g}:{} ({:.3f}%)",
                                          k ? "  " : "", lad[static_cast<std::size_t>(k)],
                                          R.bins[static_cast<std::size_t>(k)],
                                          100.0 * R.frac(k));
            log->info("Network (prior-residual census) '{}'/{}: n = {} ({} folded, {} raw-formal, "
                      "{} r-floored of which {} had post_var > fold_var, {} withheld from "
                      "σ̂₀), geo(t²) = {:.4f}.  LADDER  {}",
                      fams[f], priorResidualLabelName(lab), R.n_rows, R.n_folded,
                      R.n_raw_formal, R.n_r_clamped, R.n_r_negative, R.n_no_sigma0,
                      R.geoT2(), ladder_txt);
            if (R.max_abs_t > 0.0)
                log->info("Network (prior-residual census) '{}'/{}: worst |t| = {:.4e} at "
                          "station {} sat '{}' sig '{}' mech '{}' @TOW {:.1f} "
                          "(ê = {:.6e} m, D{{ê}} = {:.6e} m², fold_var = {:.6e}, "
                          "post_var = {:.6e}, gap = {:.1f} s).",
                          fams[f], priorResidualLabelName(lab), R.max_abs_t,
                          // -1 is a SHARED (satellite-level) prior: it has no
                          // station, which is a fact about the row, not a lookup
                          // failure.  Printing "#-1" invites the opposite reading.
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
        // ★ THE POSITIVE CONTROL (spec §9.3).  Quantified as the 4σ̂₀ ladder ratio
        // RESPONDED : CLEAN, and meaningful ONLY on the unfolded `+` branch — under
        // the folded form a refloated row's r hits the ε clamp and the "shift" is a
        // x3000 artifact, not a measurement.  A NULL result here is a PUBLISHABLE
        // FINDING, not a failed gate: it says the prior-residual statistic has no
        // power on this family, which is the same class of result as D5's 2.1 %.
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
            // The comparison is only a MEASUREMENT when both sides sit on the same
            // dispersion branch.  A RESPONDED population that is entirely unfolded
            // against a CLEAN population that is entirely folded is comparing two
            // different nulls, and the ratio is then an artifact of the branch, not
            // a statement about power.  Say so rather than let a reader quote it.
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
        // --- INC-3 contingency table (spec §4/§9.6) --------------------------
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
                // THE AUDIT that makes the b-cell above readable.  The consensus
                // rule can only move rows Stepped -> Flat, i.e. only RAISE the
                // b-cell, so quoting it without showing how much the rule moved
                // — and over what domain size — would be circular.  A mean domain
                // of 1.00 means the rule was a no-op for this family (`pl`); a
                // mean domain near the station count is the regime where v1's OR
                // saturated and the b-cell was not a rate at all.
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
                // SENSITIVITY.  Without this the consensus count is
                // UNFALSIFIABLE: "0 Stepped" reads identically whether no
                // common-mode event occurred or the rule could never have seen
                // one.  The bar is a strict majority (0.5000); this reports how
                // close the data ever came to it, which is the one number that
                // separates those two readings.
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
        log->info("Network (prior-residual census): worst |t| of the run = {:.4e} at {}.",
                  q.prires_max_t, q.prires_max_id);

    // --- the constraint-aware global variance component -----------------------
    // docs/SPEC_sigma0_constraint_aware.md §2/§6.  Invariant #4 first: the
    // DENOMINATORS and the DEGRADATION MODE before any number, because a
    // sigma0_global quoted without knowing whether its data side was r ≡ 1 (or
    // empty) is not a measurement.
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
                      "result; nothing in this run is evidence about the card.");
        } else if (!G.formed) {
            log->warn("Network (constraint-aware global σ̂₀): the run-pooled Σr is {:.6e} over "
                      "{} data + {} prior row(s) — no σ̂₀²_global was formed for the RUN.  Flags: "
                      "{}.  Check the two feed sites before reading anything else here.",
                      S.r_data + S.r_prior, S.n_data, S.n_prior, s0gFlagText(G.flags));
        } else {
            // The run-POOLED estimator (Σ over every row of the run), which is
            // the variance component; the per-epoch median/p95 that follow are a
            // different statistic and the line says so.
            // "undefined" is written as a WORD, never as a nan: a nan in a
            // run-end line reads as a numerical accident, and this one is a
            // structural statement about the run (no data side ⇒ no denominator).
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
                          "card's HEADLINE is unmeasured on this run.  That is a property of the "
                          "LINEAGE, not of the data: the post-fit RAIM pass the data side rides "
                          "is bracketed by `raim_on && carry_warm`, and carry_warm requires "
                          "network.phase_leveling.  Grade this card on a phase_leveling lineage.",
                          prires.globalQuantile(0.05), prires.globalQuantile(0.50),
                          prires.globalQuantile(0.95), prires.globalSampleCount());
        }
        // §6's per-family split: which carry family the prior-side tension came
        // from.  This is the column that has to reconcile with M2's per-family
        // robust σ̂ to ~factor 2 — a failure to reconcile is a bug in one of the
        // three instruments, and finding WHICH is the gate.
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
    // The card's own honesty line: nothing above is a defect claim.
    log->info("Network (prior-residual census): every number above is a MEASUREMENT of the "
              "carried stochastic model, not a defect claim, and no gate, deweight, exclusion "
              "or refloat was wired to any of it.  The critical value is EMPIRICAL, never "
              "tabular — a Gaussian t-table on this data rebuilds D5 v1.  The response "
              "question is a SEPARATE card whose design input is these ladders.");
}

}  // namespace GnssProcessor

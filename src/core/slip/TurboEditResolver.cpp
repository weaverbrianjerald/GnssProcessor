/**
 * @file TurboEditResolver.cpp
 * @brief Implementation of the MW+GF joint cycle-slip repair resolver: the
 *        driver of one epoch and the helpers every step shares.
 *
 * This file holds the construction and tracing, the GF combination helpers,
 * the statistics, threshold and IVI helpers, the per-epoch driver
 * @c resolve with the steps that frame the epoch (R0, R1, R8, R9), and the
 * external resets.  The other steps of @c resolve live by concern:
 *   - TurboEditChain.cpp         R2 to R4: the MW flag index and pass 1 (signal
 *                                continuity, GF chain restart, warm-up, TD2
 *                                rows, the cross-satellite pools and the IVI)
 *   - TurboEditAdjudication.cpp  R5 to R7: pass 2 for one row (its evidence,
 *                                the flagged adjudication, the trace row, the
 *                                arc update), the integer staging, and the
 *                                reconciliation into the resolution
 *   - TurboEditBridge.cpp        the gap bridge inside pass 2: the late
 *                                across-gap re-route, the bridged adjudication
 *                                and its candidate solve, the sibling-pair
 *                                affirmation
 * The working sets the steps share are the members of
 * @c TurboEditResolver::EpochWork and @c TurboEditResolver::RowWork
 * (slip/TurboEditEpochWork.hpp).
 */

#include "slip/TurboEditResolver.hpp"
#include "slip/TurboEditEpochWork.hpp"
#include "math/TimeUtils.hpp"
#include "math/RobustStats.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <utility>

namespace GnssProcessor {

// =====================================================================
// Construction
// =====================================================================

TurboEditResolver::TurboEditResolver(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      ppp_utils_(log_manager),
      sigma_multiplier_(config.getCycleSlipConfig().turbo_edit_sigma_multiplier),
      split_max_sigma_(config.getCycleSlipConfig().turbo_edit_split_max_sigma),
      mw_only_veto_(config.getCycleSlipConfig().turbo_edit_mw_only_veto),
      bridge_max_gap_s_(config.getCycleSlipConfig().turbo_edit_max_gap_s),
      wl_settle_max_s_(config.getCycleSlipConfig().turbo_edit_wl_settle_max_s),
      // The deferral parks a still-slipped ambiguity for tens of seconds.
      // That is safe only while the caller's soft-LLI holds keep its phase
      // measurements out of the filter, so it requires turbo_edit_bridge_lli.
      wl_settle_enabled_(config.getCycleSlipConfig().turbo_edit_bridge_lli &&
                         config.getCycleSlipConfig().turbo_edit_wl_settle_max_s > 0.0 &&
                         config.getCycleSlipConfig().turbo_edit_max_gap_s > 0.0) {
    log_manager_.getLogger("turbo_edit_resolver")->info(
        "TurboEditResolver initialized (MW+GF joint repair; GF-only k={:.1f}, "
        "floor={:.3f} m, min_epochs={}, frac_max={:.2f}, IVI_thresh={:.3f} m, "
        "split gate σ≤{:.2f} cyc, veto zero-gate cap={:.2f}×|λ1−λ2|, "
        "nominal high-rate={:.1f} s (advisory), mw_only_veto={} (zero gate "
        "k={:.1f}), gap_bridge={:.1f} s, k_window=±{}, wl_settle={} ({:.1f} s))",
        sigma_multiplier_, kMinThresholdM, kMinEpochsForAdaptive, kMaxFractional,
        kIviThreshold, split_max_sigma_, kVetoZeroGateMaxLambdaFrac,
        kHighRateMaxIntervalS,
        mw_only_veto_ ? "on" : "off", kCodeEventZeroSigma, bridge_max_gap_s_,
        kBridgeKWindow, wl_settle_enabled_ ? "on" : "off", wl_settle_max_s_);
}

void TurboEditResolver::setMaxGapSeconds(double seconds) {
    if (seconds > 0.0) max_gap_seconds_ = seconds;
}

// =====================================================================
// Tracing
// =====================================================================

bool TurboEditResolver::enableTracing(const std::filesystem::path& log_dir) {
    auto logger = log_manager_.getLogger("turbo_edit_resolver");

    std::error_code ec;
    if (!std::filesystem::exists(log_dir)) {
        std::filesystem::create_directories(log_dir, ec);
        if (ec) {
            logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
            return false;
        }
    }

    const auto trace_path = log_dir / "turbo_edit_trace.csv";
    trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
    if (!trace_file_.is_open()) {
        logger->warn("Could not open TurboEdit trace file '{}'", trace_path.string());
        return false;
    }

    trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                << "sig1,sig2,lambda1_m,lambda2_m,gf_m,td2_gf_m,"
                << "thresh_m,ivi_m,iono_stable,"
                << "mw_flagged,gf_flagged,dN_wl,wl_known,"
                << "dN1,dN2,frac,action,"
                << "thresh_unfloored_m,bridged,sigma_eff_m,split_gate_m\n";

    logger->info("TurboEdit tracing enabled: {}", trace_path.string());
    return true;
}

// =====================================================================
// GF combination helpers
// =====================================================================

std::string TurboEditResolver::arcKey(const std::string& sat,
                                      const std::string& sig1,
                                      const std::string& sig2) {
    return sat + "_" + sig1 + "_" + sig2;
}

std::vector<TurboEditResolver::PhasePair>
TurboEditResolver::collectPhasePairs(const PppObservation& obs) const {
    // Gather every individual carrier-phase signal (skip combined IF labels).
    std::vector<std::string> phases;
    for (const auto& [key, val] : obs.raw_observed) {
        (void)val;
        if (key.size() < 2 || key[0] != 'L') continue;
        if (key.find('_') != std::string::npos) continue;  // skip combined labels
        const char band = key[1];
        if (band < '1' || band > '9') continue;
        phases.push_back(key);
    }

    // Form all dual-band pairs, ordered higher-frequency first (matching the
    // MW detector's sig1=hi / sig2=lo convention so arc keys line up).
    std::vector<PhasePair> pairs;
    for (std::size_t i = 0; i < phases.size(); ++i) {
        for (std::size_t j = i + 1; j < phases.size(); ++j) {
            if (phases[i].size() >= 2 && phases[j].size() >= 2 &&
                phases[i][1] == phases[j][1])
                continue;  // same frequency band → GF degenerate, skip
            const double fi = ppp_utils_.getFrequency(phases[i], obs.system, obs.satellite);
            const double fj = ppp_utils_.getFrequency(phases[j], obs.system, obs.satellite);
            if (!(fi > 0.0) || !(fj > 0.0) || std::abs(fi - fj) < 1.0) continue;
            if (fi >= fj) pairs.push_back({ phases[i], phases[j] });
            else          pairs.push_back({ phases[j], phases[i] });
        }
    }
    return pairs;
}

double TurboEditResolver::computeGfMeters(const PppObservation& obs,
                                          const std::string& sig1,
                                          const std::string& sig2) const {
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    auto it1 = obs.raw_observed.find(sig1);
    auto it2 = obs.raw_observed.find(sig2);
    if (it1 == obs.raw_observed.end() || it2 == obs.raw_observed.end()) return NaN;

    const double L1_cyc = it1->second;
    const double L2_cyc = it2->second;
    if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc)) return NaN;

    const double lam1 = ppp_utils_.getWavelength(obs.system, sig1, obs.satellite);
    const double lam2 = ppp_utils_.getWavelength(obs.system, sig2, obs.satellite);
    if (lam1 <= 0.0 || lam2 <= 0.0) return NaN;

    // GF = L1 - L2 (metres): geometry-free, carries ionosphere + ambiguity.
    const double gf = L1_cyc * lam1 - L2_cyc * lam2;
    return std::isfinite(gf) ? gf : NaN;
}

// =====================================================================
// Statistics / threshold / IVI
// =====================================================================

double TurboEditResolver::computeMadSigma(const std::deque<double>& window) {
    if (window.size() < kMinEpochsForAdaptive)
        return std::numeric_limits<double>::quiet_NaN();
    return RobustStats::madSigma(window);
}

double TurboEditResolver::effectiveSigma(const GfArc& arc,
                                         const std::string& arc_key,
                                         double elev_deg) const {
    // Same split as the detection threshold (computeAdaptiveThreshold): the
    // arc's own MAD once mature, the cross-sat σ during warmup.  Returns the
    // raw σ, because the rate-adaptive gates compare it against lattice
    // geometry rather than against a kσ detection multiple.
    const double own = computeMadSigma(arc.diff_window);
    if (std::isfinite(own)) return own;
    return xsatSigmaFloor(arc_key, elev_deg);
}

bool TurboEditResolver::splitCapable(double sigma_m, double denom_m) const {
    // NaN σ fails by design: "no noise estimate" is not evidence of
    // sharpness, and the only NaN window is the true cold start (the MAD
    // window survives slips and short gaps; the cross-sat cascade covers
    // fresh arcs from epoch ~2 of a run).
    return std::isfinite(sigma_m) && sigma_m >= 0.0 &&
           std::abs(denom_m) >= 1e-6 &&
           sigma_m <= split_max_sigma_ * std::abs(denom_m);
}

double TurboEditResolver::computeAdaptiveThreshold(const GfArc& arc,
                                                   const std::string& arc_key,
                                                   double elev_deg,
                                                   double* out_unfloored) const {
    // Mature arc: its own MAD governs; the cross-sat σ is used only in warmup.
    const double sigma = computeMadSigma(arc.diff_window);
    if (std::isfinite(sigma)) {
        if (out_unfloored) *out_unfloored = sigma_multiplier_ * sigma;
        return std::max(sigma_multiplier_ * sigma, kMinThresholdM);
    }

    // Warmup: borrow the cross-sat TD2 noise scale so a fresh arc (cold start,
    // after a gap, after a slip) has a finite, statistically based gate from
    // its first TD2.  On quiet data k×σ_xsat falls below the quantum floor and
    // the floor governs, which is correct because the smallest GF event is
    // |λ1−λ2| ≈ 5.4 cm (GPS L1/L2).  The pool matters when it raises fresh-arc
    // gates during ionospheric activity or on noisy receivers, where the bare
    // floor would raise false flags and resets.  With the cascade (band-pair,
    // constellation, global non-GLONASS), the bare floor gates alone only at
    // the cold start (the first ~2 epochs of a run) and for GLONASS when its
    // own tiers are thin.  The result is floor-limited and never NaN.
    const double xsat = xsatSigmaFloor(arc_key, elev_deg);
    if (std::isfinite(xsat)) {
        if (out_unfloored) *out_unfloored = sigma_multiplier_ * xsat;
        return std::max(sigma_multiplier_ * xsat, kMinThresholdM);
    }
    if (out_unfloored) *out_unfloored = std::numeric_limits<double>::quiet_NaN();
    return kMinThresholdM;
}

std::string TurboEditResolver::xsatGroupKey(const std::string& arc_key) {
    // "G01_L1C_L2W" → "G_L1_L2": pool by frequency pair with the tracking
    // attribute collapsed (mirrors MwSlipDetector's cross-sat group key).
    if (arc_key.empty()) return {};
    const auto us1 = arc_key.find('_');
    if (us1 == std::string::npos || us1 == 0) return {};
    const std::string rest = arc_key.substr(us1 + 1);  // "<sig1>_<sig2>"
    const auto us2 = rest.find('_');
    if (us2 == std::string::npos) return {};
    const std::string sig1 = rest.substr(0, us2);
    const std::string sig2 = rest.substr(us2 + 1);
    if (sig1.size() < 2 || sig2.size() < 2) return {};
    return std::string(1, arc_key.front()) + "_"
         + sig1.substr(0, 2) + "_" + sig2.substr(0, 2);
}

double TurboEditResolver::xsatSigmaFloor(const std::string& arc_key,
                                         double elev_deg) const {
    if (arc_key.empty()) return std::numeric_limits<double>::quiet_NaN();
    double sigma = std::numeric_limits<double>::quiet_NaN();

    const std::string gkey = xsatGroupKey(arc_key);
    if (!gkey.empty()) {                                  // Tier 1: band-pair
        auto it = xsat_sigma_group_.find(gkey);
        if (it != xsat_sigma_group_.end() && std::isfinite(it->second))
            sigma = it->second;
    }
    if (!std::isfinite(sigma)) {                          // Tier 2: constellation
        auto it = xsat_sigma_cons_.find(std::string(1, arc_key.front()));
        if (it != xsat_sigma_cons_.end() && std::isfinite(it->second))
            sigma = it->second;
    }
    if (!std::isfinite(sigma) && arc_key.front() != 'R'   // Tier 3: global
        && std::isfinite(xsat_sigma_global_)) {           // (non-GLONASS, as in MW)
        sigma = xsat_sigma_global_;
    }
    if (!std::isfinite(sigma)) return sigma;
    return sigma * turbo_edit::gfElevWeight(elev_deg);  // re-expand from zenith reference
}

void TurboEditResolver::finalizeXsatSigmas() {
    // EWMA blend per tier; the first finite MAD initializes it.  A tier
    // updates only when the epoch's pool reaches kMinXsatSamples; otherwise
    // the previous value persists, so a thin cross-section cannot swing the
    // floor.  Mirrors MwSlipDetector::finalizeCrossSatSigmas, including the
    // tier-3 global non-GLONASS fallback: a constellation with 1-2 visible
    // satellites never reaches kMinXsatSamples in its own tiers and would
    // otherwise stay at the bare quantum floor for the whole run.
    const auto blend = [](double prev, double raw) {
        return std::isfinite(prev)
                   ? kXsatEwmaAlpha * raw + (1.0 - kXsatEwmaAlpha) * prev
                   : raw;
    };
    // Narrow-spacing pairs (intra-E5/B2 family: both bands in {L5,L7,L8})
    // have sub-mm TD2 noise, an order of magnitude below the cm-scale wide
    // pairs.  They keep their own tier-1 group but are excluded from the
    // coarse tiers, which they would otherwise pull toward the small end,
    // reducing the lift those tiers provide.
    const auto isNarrowPair = [](const std::string& gkey) {
        // gkey = "<cons>_<b1>_<b2>", bands are 2 chars ("L5"/"L7"/"L8").
        if (gkey.size() != 7) return false;
        const auto narrow = [](char d) { return d == '5' || d == '7' || d == '8'; };
        return narrow(gkey[3]) && narrow(gkey[6]);
    };

    std::map<std::string, std::vector<double>> cons_devs;
    std::vector<double> global_devs;
    for (auto& [gkey, devs] : xsat_devs_) {
        if (gkey.empty() || devs.empty()) continue;
        if (!isNarrowPair(gkey)) {
            auto& cd = cons_devs[std::string(1, gkey.front())];
            cd.insert(cd.end(), devs.begin(), devs.end());
            if (gkey.front() != 'R')
                global_devs.insert(global_devs.end(), devs.begin(), devs.end());
        }

        if (devs.size() < kMinXsatSamples) continue;
        const double mad = RobustStats::madSigma(devs);
        if (std::isfinite(mad) && mad > 0.0) {
            auto it = xsat_sigma_group_.find(gkey);
            const double prev = (it != xsat_sigma_group_.end())
                                    ? it->second
                                    : std::numeric_limits<double>::quiet_NaN();
            xsat_sigma_group_[gkey] = blend(prev, mad);
        }
    }
    for (auto& [cons, cd] : cons_devs) {
        if (cd.size() < kMinXsatSamples) continue;
        const double mad = RobustStats::madSigma(cd);
        if (std::isfinite(mad) && mad > 0.0) {
            auto it = xsat_sigma_cons_.find(cons);
            const double prev = (it != xsat_sigma_cons_.end())
                                    ? it->second
                                    : std::numeric_limits<double>::quiet_NaN();
            xsat_sigma_cons_[cons] = blend(prev, mad);
        }
    }
    if (global_devs.size() >= kMinXsatSamples) {
        const double mad = RobustStats::madSigma(global_devs);
        if (std::isfinite(mad) && mad > 0.0)
            xsat_sigma_global_ = blend(xsat_sigma_global_, mad);
    }
    xsat_devs_.clear();
}

double TurboEditResolver::computeIvi(const std::vector<double>& constellation_td2) {
    if (constellation_td2.size() < 3) return 0.0;
    std::vector<double> abs_vals;
    abs_vals.reserve(constellation_td2.size());
    for (const double v : constellation_td2) abs_vals.push_back(std::abs(v));
    return RobustStats::median(abs_vals) + RobustStats::madSigma(abs_vals);
}

// =====================================================================
// resolve(): the per-epoch driver
// =====================================================================

TurboEditResolver::EpochResolution TurboEditResolver::resolve(
    const std::vector<MwSlipResult>& mw_results,
    const std::set<std::string>& mw_clean_pairs,
    const std::vector<PppObservation>& observations,
    double interval_s,
    const std::map<std::string, MwSlipDetector::AcrossGapWlEvidence>&
        wl_gap_evidence) {
    EpochWork w(mw_results, mw_clean_pairs, observations, wl_gap_evidence,
                log_manager_.getLogger("turbo_edit_resolver"));

    adviseCoarseRate(w, interval_s);   // R0  one-time coarse-rate advisory
    stampEpoch(w);                     // R1  epoch time (tracing, continuity)
    indexMwFlags(w);                   // R2  MW wide-lane flags by arc key
    collectChainRows(w);               // R3  pass 1: GF chain, TD2 rows, pools
    assessIonosphere(w);               // R4  cross-satellite IVI per constellation
    for (const auto& cached : w.cache)
        adjudicateRow(w, cached);      // R5  pass 2: one row (A0 to A5)
    reconcileStagedIntegers(w);        // R6  staged integers to repairs, clean, moot
    withdrawContradictedVerdicts(w);   // R7  code events and pending of reset sats
    logEpochSummary(w);                // R8  the per-epoch info line
    finishEpoch(w);                    // R9  cross-sat tiers, epoch marker, flush
    return std::move(w.out);
}

// =====================================================================
// R0, R1, R8, R9: the epoch frame
// =====================================================================

void TurboEditResolver::adviseCoarseRate(EpochWork& w, double interval_s) {
    const auto& log = w.log;
    auto& out = w.out;

    // --- Coarse-rate advisory (informational; decisions are evidence-gated) ---
    if (interval_s > 0.0 && interval_s > kHighRateMaxIntervalS) {
        out.rate_ok = false;
        if (!rate_warned_) {
            log->info("TurboEdit: sampling interval {:.3f} s exceeds the nominal "
                      "high-rate regime ({:.1f} s) — the geometry-free jump "
                      "carries an un-cancelled iono residual at this rate.  "
                      "Operating rate-adaptively: per-signal repairs, GF-only "
                      "splits and code-event vetoes engage only while the "
                      "measured GF noise stays sharp against the integer "
                      "lattice (split gate σ≤{:.2f}×|λ1−λ2|); otherwise "
                      "MW-flagged satellites fall back to reset and GF-only "
                      "exceedances are left to the KF residual backstop.",
                      interval_s, kHighRateMaxIntervalS, split_max_sigma_);
            rate_warned_ = true;
        }
    }
}

void TurboEditResolver::stampEpoch(EpochWork& w) {
    const auto& observations = w.observations;
    auto& epoch_now_s = w.epoch_now_s;

    // Capture epoch time for tracing and consecutive-epoch detection.
    for (const auto& obs : observations)
        if (!obs.satellite.empty()) {
            current_epoch_ = obs.time;
            epoch_now_s = TimeUtils::continuousGpsSeconds(obs.time);
            break;
        }
}

void TurboEditResolver::logEpochSummary(const EpochWork& w) const {
    const auto& log = w.log;
    const auto& out = w.out;

    if (!out.repairs.empty() || !out.reset_sats.empty() ||
        !out.code_events.empty() || !out.moot_signals.empty() ||
        !out.pending_sats.empty()) {
        log->info("TurboEdit epoch: {} per-signal repair(s), {} reset(s), "
                  "{} code-domain veto(es), {} moot signal set(s), "
                  "{} WL-settle pending, iono_stable={}, max_IVI={:.4f} m",
                  out.repairs.size(), out.reset_sats.size(), out.code_events.size(),
                  out.moot_signals.size(), out.pending_sats.size(),
                  out.iono_stable ? "yes" : "no", out.max_ivi);
    }
}

void TurboEditResolver::finishEpoch(const EpochWork& w) {
    const auto& epoch_now_s = w.epoch_now_s;

    // Fold this epoch's TD2 noise pool into the EWMA cross-sat tiers for the
    // next epoch's warmup floors (a one-epoch lag by construction: the pool
    // is not complete until every pair has been processed).
    finalizeXsatSigmas();

    // Advance the previous-epoch marker so the next call can detect per-signal
    // dropouts.  Only when this epoch actually carried observations (an empty
    // epoch leaves the marker so a returning arc is still seen as a gap).
    if (epoch_now_s > 0.0) prev_epoch_s_ = epoch_now_s;

    // Per-epoch flush so the trace survives non-graceful termination.  This
    // stream writes far fewer bytes per epoch than mwwl_trace.csv, so its
    // buffer spans many epochs.  Without an explicit flush, a killed run
    // loses a multi-epoch tail and the file ends mid-row, out of step with
    // the MW trace by however much data each buffer held.
    if (trace_file_.is_open()) trace_file_.flush();
}

// =====================================================================
// External reset
// =====================================================================

void TurboEditResolver::markHeldAcrossLli(const std::string& satellite,
                                         const std::string& signal) {
    if (satellite.empty() || signal.empty()) return;
    held_across_lli_.insert(satellite + "_" + signal);
}

void TurboEditResolver::resetSatellite(const std::string& satellite) {
    const std::string prefix = satellite + "_";
    for (auto it = gf_arcs_.begin(); it != gf_arcs_.end(); ) {
        if (it->first.rfind(prefix, 0) == 0) it = gf_arcs_.erase(it);
        else ++it;
    }
    // A hard arc reset for the sat voids any held-across-LLI assertion for it
    // (the GF level reference the bridge would repair against is gone).
    for (auto it = held_across_lli_.begin(); it != held_across_lli_.end(); ) {
        if (it->rfind(prefix, 0) == 0) it = held_across_lli_.erase(it);
        else ++it;
    }
}

void TurboEditResolver::resetSatelliteSignal(const std::string& satellite,
                                             const std::string& signal) {
    // Drop arcs whose key contains "<sat>_..._<signal>_..." touching this signal.
    const std::string sat_prefix = satellite + "_";
    for (auto it = gf_arcs_.begin(); it != gf_arcs_.end(); ) {
        const std::string& k = it->first;
        const bool sat_match = (k.rfind(sat_prefix, 0) == 0);
        const bool sig_match =
            (k.find("_" + signal + "_") != std::string::npos) ||
            (k.size() >= signal.size() + 1 &&
             k.compare(k.size() - signal.size() - 1, signal.size() + 1, "_" + signal) == 0);
        if (sat_match && sig_match) it = gf_arcs_.erase(it);
        else ++it;
    }
    // Drop the held-across-LLI assertion for this exact (sat, signal).
    held_across_lli_.erase(satellite + "_" + signal);
}

} // namespace GnssProcessor

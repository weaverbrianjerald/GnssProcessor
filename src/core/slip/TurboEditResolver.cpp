/**
 * @file TurboEditResolver.cpp
 * @brief Implementation of the MW+GF joint cycle-slip repair resolver.
 *
 * Organization:
 *   - Construction / tracing
 *   - GF combination helpers
 *   - Statistics / threshold / IVI
 *   - resolve(): two-pass (collect TD2 → assess IVI → flag + joint solve)
 *   - External reset
 */

#include "slip/TurboEditResolver.hpp"
#include "math/TimeUtils.hpp"
#include "math/RobustStats.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

namespace {
/// Satellite id from an MW result (prefer the field; fall back to amb_key prefix).
std::string satOf(const MwSlipResult& sr) {
    if (!sr.satellite.empty()) return sr.satellite;
    const auto p = sr.amb_key.find('_');
    return (p != std::string::npos) ? sr.amb_key.substr(0, p) : sr.amb_key;
}

/// Elevation shaping for the cross-sat TD2 pool: 1/sin(el), with el clamped at
/// 10° so a near-horizon arc cannot produce an unbounded weight.  Mirrors the MW
/// detector's mwElevationWeight.  The pool stores zenith-normalized values and
/// the floor re-expands them to the consuming arc's elevation, so elevation sets
/// the shape of the floor while its magnitude stays data-driven.
double gfElevWeight(double elev_deg) {
    constexpr double kMinElevDeg = 10.0;
    constexpr double kDegToRad   = 0.017453292519943295;
    const double el = (std::isfinite(elev_deg) && elev_deg > kMinElevDeg)
                          ? elev_deg : kMinElevDeg;
    return 1.0 / std::sin(el * kDegToRad);
}

/// Receiver-declared loss of lock on either phase signal of a pair.
/// Mirrors MwSlipDetector::hasAnyPhaseLockLoss's `lli != 0` convention.
/// A set LLI corroborates that integer continuity broke, so the code-domain
/// veto does not apply; the caller's LLI handling governs.
bool pairHasLockLoss(const PppObservation& obs,
                     const std::string& sig1, const std::string& sig2) {
    const auto set = [&obs](const std::string& sig) {
        const auto it = obs.lli_flags.find(sig);
        return it != obs.lli_flags.end() && it->second != 0;
    };
    return set(sig1) || set(sig2);
}
}  // namespace

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
    return sigma * gfElevWeight(elev_deg);  // re-expand from zenith reference
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
// resolve(): two-pass joint solve
// =====================================================================

TurboEditResolver::EpochResolution TurboEditResolver::resolve(
    const std::vector<MwSlipResult>& mw_results,
    const std::set<std::string>& mw_clean_pairs,
    const std::vector<PppObservation>& observations,
    double interval_s,
    const std::map<std::string, MwSlipDetector::AcrossGapWlEvidence>&
        wl_gap_evidence) {

    auto log = log_manager_.getLogger("turbo_edit_resolver");
    EpochResolution out;

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

    // Capture epoch time for tracing and consecutive-epoch detection.
    double epoch_now_s = 0.0;
    for (const auto& obs : observations)
        if (!obs.satellite.empty()) {
            current_epoch_ = obs.time;
            epoch_now_s = TimeUtils::continuousGpsSeconds(obs.time);
            break;
        }

    // --- Index MW wide-lane slips by exact pair key (dedup the two-per-pair entries) ---
    struct MwFlag {
        int    dN_wl{ 0 };
        bool   known{ false };
        /// Float WL-step estimate (cycles, ZD frame).  Selects candidates for
        /// the gap-bridge solve when the integer is withheld (for example on
        /// the locked-arc TD-confirmation path).  NaN when unavailable.
        double wl_float{ std::numeric_limits<double>::quiet_NaN() };
    };
    std::map<std::string, MwFlag> mw_by_pair;
    for (const auto& sr : mw_results) {
        if (sr.sig1.empty() || sr.sig2.empty()) continue;  // LLI / signal-less: not a pair slip
        const std::string sat = satOf(sr);
        if (sat.empty()) continue;
        auto& f = mw_by_pair[arcKey(sat, sr.sig1, sr.sig2)];
        // Prefer any entry that carries a usable integer for this pair.
        if (sr.slip_wl_known && !f.known) { f.dN_wl = sr.slip_wl_cycles; f.known = true; }
        if (!std::isfinite(f.wl_float) && std::isfinite(sr.slip_wl_float))
            f.wl_float = sr.slip_wl_float;
    }

    // --- Pass 1: GF + TD2 for every dual-band phase pair; collect IVI pools ---
    struct PairEpoch {
        const PppObservation* obs;
        std::string key, sig1, sig2;
        double gf{ 0.0 }, td2{ 0.0 }, lambda1{ 0.0 }, lambda2{ 0.0 };
        double thresh{ std::numeric_limits<double>::quiet_NaN() };
        double thresh_unfloored{ std::numeric_limits<double>::quiet_NaN() };  // kσ·MAD, no floor (trace diagnostic)
        bool   gf_exceeds{ false };  // |TD2| > thresh (outlier, ignoring cooldown)
        /// True when @c td2 holds not a TD2 but the drift-corrected across-gap
        /// jump from a stashed pre-gap level (see the header's gap-bridge
        /// note); @c gap_s is the span back to the stashed reference.
        bool   bridged{ false };
        double gap_s{ 0.0 };
    };
    std::vector<PairEpoch> cache;
    std::map<char, std::vector<double>> cons_td2;

    for (const auto& obs : observations) {
        if (obs.satellite.empty()) continue;
        if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
            continue;
        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // Per-signal phase-continuity bookkeeping for the gap bridge's
        // moot-vs-repair dispatch (observation-domain; below-mask epochs
        // were excluded above and therefore count as missing, matching the
        // filter's own usage of these observations).
        for (const auto& [sig, val] : obs.raw_observed) {
            if (sig.size() < 2 || sig[0] != 'L') continue;
            if (sig.find('_') != std::string::npos) continue;
            if (!std::isfinite(val)) continue;
            const std::string skey = obs.satellite + "_" + sig;
            const auto ls = sig_last_seen_s_.find(skey);
            if (ls == sig_last_seen_s_.end() ||
                (prev_epoch_s_ > 0.0 && ls->second < prev_epoch_s_ - 1e-6)) {
                sig_continuous_since_s_[skey] = epoch_s;
            }
            sig_last_seen_s_[skey] = epoch_s;
        }

        for (const auto& pr : collectPhasePairs(obs)) {
            const double gf = computeGfMeters(obs, pr.phase1, pr.phase2);
            if (!std::isfinite(gf)) continue;
            const double lam1 = ppp_utils_.getWavelength(obs.system, pr.phase1, obs.satellite);
            const double lam2 = ppp_utils_.getWavelength(obs.system, pr.phase2, obs.satellite);
            if (!(lam1 > 0.0) || !(lam2 > 0.0)) continue;

            const std::string key = arcKey(obs.satellite, pr.phase1, pr.phase2);
            auto& arc = gf_arcs_[key];

            // Restart the arc when its history is not strictly consecutive, on
            // either of two triggers:
            //  (a) Absolute gap: elapsed time exceeds the configured tolerance.
            //  (b) Per-signal dropout: the arc did not update in the previous
            //      epoch (its last update predates prev_epoch_s_).  This pair
            //      was missing while other satellites were observed, so its
            //      prev_gf / prev_prev_gf reference non-adjacent epochs and the
            //      2nd-order time difference would be malformed.  An exact
            //      timestamp comparison makes this rate-agnostic (works at 1 Hz
            //      or 10 Hz) without depending on max_gap_seconds_.
            // Trigger (a) has no count>0 requirement: a post-slip arc has
            // count==0 with a preserved MAD window and a stamped last_epoch_s,
            // and the absolute-gap trigger stays armed so that a long dropout
            // after the slip expires the preserved calibration on return.
            // (Brand-new arcs have last_epoch_s == 0 and are unaffected.)
            const bool absolute_gap =
                arc.last_epoch_s > 0.0 &&
                (epoch_s - arc.last_epoch_s) > max_gap_seconds_;
            const bool skipped_prev_epoch =
                arc.count > 0 && arc.last_epoch_s > 0.0 && prev_epoch_s_ > 0.0 &&
                arc.last_epoch_s < prev_epoch_s_ - 1e-6;
            // Gap bridge: a chain break within turbo_edit_max_gap_s stashes the
            // pre-gap level (requires count >= 2, a level confirmed by at least
            // one consecutive pair) so the across-gap jump can adjudicate
            // post-gap MW flags.  It also preserves the MAD calibration window:
            // a 15 s gap changes the arc's TD2 noise no more than the 1.5 s
            // gaps (max_gap_seconds_ = 1.5 × interval at 1 Hz) that the partial
            // reset already tolerates.  Beyond the cap the two-tier behavior
            // governs: an absolute gap wipes the arc; a per-signal dropout
            // inside max_gap_seconds_ gets a partial reset.  A break during
            // warm-up (count < 2) cannot stash and also voids any older bridge,
            // so bridges do not compound across repeated breaks.
            const double gap_span_s =
                (arc.last_epoch_s > 0.0) ? (epoch_s - arc.last_epoch_s) : 0.0;
            const bool bridgeable =
                (absolute_gap || skipped_prev_epoch) &&
                bridge_max_gap_s_ > 0.0 &&
                gap_span_s > 0.0 && gap_span_s <= bridge_max_gap_s_;
            if (bridgeable) {
                if (arc.count >= 2) {
                    arc.bridge_valid    = true;
                    arc.bridge_gf       = arc.prev_gf;
                    arc.bridge_epoch_s  = arc.last_epoch_s;
                    arc.bridge_rate_mps = arc.rate_ewma_mps;
                    arc.bridge_dt0_s    = arc.last_spacing_s;
                } else {
                    arc.bridge_valid = false;
                }
                arc.prev_gf            = 0.0;
                arc.prev_prev_gf       = 0.0;
                arc.count              = 0;
                arc.last_epoch_s       = 0.0;
                arc.cooldown_remaining = 0;
                arc.rate_ewma_mps  = std::numeric_limits<double>::quiet_NaN();
                arc.last_spacing_s = 0.0;
            } else if (absolute_gap) {
                arc = GfArc{};
            } else if (skipped_prev_epoch) {
                // Partial reset: the TD2 chain is non-consecutive and re-seeds
                // (prev_gf / prev_prev_gf reference non-adjacent epochs), but
                // the MAD window is calibration, not chain state.  The TD2
                // noise level is a slowly varying property of the arc
                // (elevation, tracking, multipath) that a gap inside
                // max_gap_seconds_ does not invalidate.  Wiping it with the
                // chain would leave kMinEpochsForAdaptive (5) extra epochs
                // without a MAD threshold after every short dropout, which
                // are the epochs where post-gap slips need a live GF gate.
                // Keeping it adds no cross-gap differencing: the re-seeded
                // chain builds the first post-gap TD2 from post-gap samples
                // only, and the preserved window only calibrates the gate that
                // tests it.  Cooldown clears, because a pre-gap slip's TD2
                // echo cannot form across a re-seeded chain.
                arc.prev_gf            = 0.0;
                arc.prev_prev_gf       = 0.0;
                arc.count              = 0;
                arc.last_epoch_s       = 0.0;
                arc.cooldown_remaining = 0;
                // With bridging on, this branch is reached only when the gap
                // exceeded the bridge cap, so the stale level is invalidated.
                arc.bridge_valid   = false;
                arc.rate_ewma_mps  = std::numeric_limits<double>::quiet_NaN();
                arc.last_spacing_s = 0.0;
            }

            // Warm-up epochs (count < 2): no TD2 yet.  With a fresh bridge the
            // pair is cached against the drift-corrected across-gap jump
            // instead (pass 2 adjudicates it and rolls the chain, as for
            // normal cached rows); otherwise the chain rolls forward inline.
            if (arc.count < 2) {
                bool cached_bridged = false;
                if (arc.bridge_valid) {
                    const double span_s = epoch_s - arc.bridge_epoch_s;
                    if (span_s > 0.0 && span_s <= bridge_max_gap_s_) {
                        const double drift =
                            std::isfinite(arc.bridge_rate_mps)
                                ? arc.bridge_rate_mps * span_s : 0.0;
                        const double dgf_gap = gf - arc.bridge_gf - drift;
                        // Gate from the preserved calibration, widened by the
                        // random-walk factor √(Δt/dt₀): the arc's own
                        // per-interval variability extrapolated over the gap.
                        double thresh_unfloored =
                            std::numeric_limits<double>::quiet_NaN();
                        const double base = computeAdaptiveThreshold(
                            arc, key, obs.elevation, &thresh_unfloored);
                        const double widen =
                            (arc.bridge_dt0_s > 0.0 && span_s > arc.bridge_dt0_s)
                                ? std::sqrt(span_s / arc.bridge_dt0_s) : 1.0;
                        const bool exceeds = std::isfinite(base) &&
                                             std::abs(dgf_gap) > base * widen;
                        // Bridged rows feed neither the IVI pool nor the
                        // cross-sat TD2 pool: the across-gap jump is not a
                        // TD2 sample and would bias both statistics.
                        cache.push_back({ &obs, key, pr.phase1, pr.phase2, gf,
                                          dgf_gap, lam1, lam2,
                                          base * widen, thresh_unfloored * widen,
                                          exceeds, /*bridged=*/true, span_s });
                        cached_bridged = true;
                    } else {
                        arc.bridge_valid = false;  // span exceeded the cap
                    }
                }
                if (!cached_bridged) {
                    if (arc.count == 1 && arc.last_epoch_s > 0.0 &&
                        epoch_s > arc.last_epoch_s) {
                        const double dt = epoch_s - arc.last_epoch_s;
                        const double rate = (gf - arc.prev_gf) / dt;
                        arc.rate_ewma_mps = std::isfinite(arc.rate_ewma_mps)
                            ? kBridgeRateAlpha * rate +
                                  (1.0 - kBridgeRateAlpha) * arc.rate_ewma_mps
                            : rate;
                        arc.last_spacing_s = dt;
                    }
                    if (arc.count == 0) arc.prev_gf = gf;
                    else { arc.prev_prev_gf = arc.prev_gf; arc.prev_gf = gf; }
                    ++arc.count;
                    arc.last_epoch_s = epoch_s;
                    if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
                }
                continue;
            }

            const double td2    = gf - 2.0 * arc.prev_gf + arc.prev_prev_gf;
            double thresh_unfloored = std::numeric_limits<double>::quiet_NaN();
            const double thresh = computeAdaptiveThreshold(arc, key, obs.elevation,
                                                           &thresh_unfloored);
            const bool gf_exceeds = std::isfinite(thresh) && std::abs(td2) > thresh;

            // The chain has a TD2 again, but the pre-gap stash stays valid
            // (time-bounded) until consumed or expired.  The MW locked-arc
            // TD-confirmation path delivers its across-gap flag one or more
            // epochs after the chain regains a TD2.  That flag is adjudicated
            // against the across-gap jump, because the rebuilt chain's TD2
            // cannot see it by construction (a flat post-gap TD2 would pass a
            // genuine re-lock slip to the MW-only veto as a "code event").
            // Pass 2 routes such late MW flags back to the stash.  The stash
            // expires at the cap, measured from the pre-gap level's epoch,
            // extended by the WL-settle window (turbo_edit_wl_settle_max_s)
            // while a deferred adjudication waits on the post-restart MW mean.
            {
                const double stash_cap = arc.wl_settle_pending
                    ? bridge_max_gap_s_ + wl_settle_max_s_
                    : bridge_max_gap_s_;
                if (arc.bridge_valid &&
                    (epoch_s - arc.bridge_epoch_s) > stash_cap) {
                    arc.bridge_valid      = false;
                    arc.wl_settle_pending = false;
                }
            }

            cache.push_back({ &obs, key, pr.phase1, pr.phase2, gf, td2, lam1, lam2,
                              thresh, thresh_unfloored, gf_exceeds });
            cons_td2[obs.satellite[0]].push_back(td2);

            // Cross-sat TD2 noise pool (source of the warmup floor): samples
            // are zenith-normalized so the pooled MAD is a homoscedastic
            // estimate (xsatSigmaFloor re-expands per arc).  Exceeding samples
            // are excluded, because a slip TD2 is a discrete event, not noise.
            if (!gf_exceeds) {
                const std::string gkey = xsatGroupKey(key);
                if (!gkey.empty())
                    xsat_devs_[gkey].push_back(td2 / gfElevWeight(obs.elevation));
            }
        }
    }

    // --- Cross-satellite IVI per constellation; global stability flag ---
    std::map<char, double> ivi_by_cons;
    double max_ivi = 0.0;
    for (const auto& [cons, td2v] : cons_td2) {
        const double ivi = computeIvi(td2v);
        ivi_by_cons[cons] = ivi;
        max_ivi = std::max(max_ivi, ivi);
    }
    out.max_ivi = max_ivi;
    out.iono_stable = (max_ivi < kIviThreshold);

    // (No high-rate affirmation: GF trust is evidence-gated per arc.  The
    // split-capability gate and the veto zero-gate cap scale with the
    // measured TD2 noise, which the un-cancelled coarse-rate iono residual
    // inflates.  A 30 s file is therefore demoted as far as its own noise
    // requires, with or without an INTERVAL header.)

    // --- Pass 2: flag (MW ∪ GF-only), joint solve, update arc ---
    // Per-(sat, signal) integer candidates, reconciled across a sat's pairs.
    std::map<std::string, std::map<std::string, int>>    cand;
    std::map<std::string, std::map<std::string, double>> cand_lambda;
    std::map<std::string, std::map<std::string, double>> cand_frac;
    // Bridged-moot markers: the staged integer is the signal's true
    // across-gap slip (so cross-pair consistency checks stay valid), but the
    // signal missed an epoch during the gap.  Its ambiguity was pruned and
    // reseeded and already absorbs the slip, so emission moves it from
    // repairs to EpochResolution::moot_signals (mandate suppression, no state
    // action).
    std::map<std::string, std::set<std::string>> cand_moot;
    std::set<std::string> conflict_sats;

    // Row lookup by arc key for the sibling-pair affirmation below.  Rows
    // are this epoch's snapshot; pass 2 mutates arcs, never `cache`.
    std::map<std::string, const PairEpoch*> pe_by_key;
    for (const auto& row : cache) pe_by_key[row.key] = &row;

    // --- Sibling-pair zero affirmation (gap-bridge constraint) -----------
    // The GF equation is degenerate along the null-line lattice: bridged
    // candidates k and k±2 can fit the same across-gap jump within mm, and
    // only the MW evidence (biased by code noise at re-acquisition) separates
    // them.  Another GF arc of the same satellite often carries the missing
    // information.  An arc that (a) has cross-frequency leverage, (b) stayed
    // chain-continuous through the bridged interval (no break, so no blind
    // gap; a slip on either signal at any epoch of the interval would have
    // flagged its TD2), (c) is calibration-mature, and (d) has no current
    // exceedance or cooldown affirms dN = 0 for both of its signals.
    // Example: an L1C/L2L chain that runs unbroken through an L2W outage
    // affirms dN(L1C) = 0, which selects k = +28, (0, −28) over the
    // float-favored k = +26, (−9, −35).  Residual risk: a simultaneous
    // null-line pair on the sibling itself is invisible to GF, the same
    // documented exposure as the code-event veto, backstopped by the KF
    // innovation gate.
    auto affirmedZeroSignals = [&](const std::string& sat,
                                   const std::string& exclude_key,
                                   double ref_epoch_s) -> std::set<std::string> {
        std::set<std::string> affirmed;
        const std::string prefix = sat + "_";
        for (const auto& [k2, row2] : pe_by_key) {
            if (k2 == exclude_key) continue;
            if (k2.rfind(prefix, 0) != 0) continue;
            const PairEpoch& q = *row2;
            if (q.bridged) continue;                 // its own chain broke
            if (q.gf_exceeds) continue;              // flagged this epoch
            if (std::abs(q.lambda1 - q.lambda2) < 1e-6) continue;  // no GF leverage
            const auto a2 = gf_arcs_.find(k2);
            if (a2 == gf_arcs_.end()) continue;
            const GfArc& arc2 = a2->second;
            if (arc2.cooldown_remaining > 0) continue;             // recent event
            if (arc2.diff_window.size() < kCodeEventMinMadSamples) continue;
            // Chain continuity back past the pre-gap reference: the
            // current unbroken run must have started at or before it.
            if (!(arc2.last_spacing_s > 0.0) || arc2.count < 2) continue;
            const double chain_start = arc2.last_epoch_s
                - static_cast<double>(arc2.count - 1) * arc2.last_spacing_s;
            if (chain_start > ref_epoch_s + 1e-6) continue;
            // Observation-domain continuity of both signals through the
            // interval (below-mask epochs count as missing).
            const auto c1 = sig_continuous_since_s_.find(prefix + q.sig1);
            const auto c2 = sig_continuous_since_s_.find(prefix + q.sig2);
            if (c1 == sig_continuous_since_s_.end() ||
                c1->second > ref_epoch_s + 1e-6) continue;
            if (c2 == sig_continuous_since_s_.end() ||
                c2->second > ref_epoch_s + 1e-6) continue;
            affirmed.insert(q.sig1);
            affirmed.insert(q.sig2);
        }
        return affirmed;
    };

    for (auto pe : cache) {  // by value: the late-flag re-route below mutates the row
        auto& arc = gf_arcs_[pe.key];
        const std::string& sat = pe.obs->satellite;
        const char cons = sat[0];
        const double ivi = ivi_by_cons.count(cons) ? ivi_by_cons.at(cons) : 0.0;
        const bool iono_stable_cons = (ivi < kIviThreshold);

        // Rate-adaptive evidence for this arc (header's "Rate adaptivity"
        // note): the raw TD2 noise σ and the lattice spacing it must
        // discriminate.  sigma_trace is overwritten with the gap-widened
        // value on bridged adjudications; both feed the trace.
        const double denom_row    = pe.lambda1 - pe.lambda2;
        const double split_gate_m = split_max_sigma_ * std::abs(denom_row);
        double sigma_trace = effectiveSigma(arc, pe.key, pe.obs->elevation);

        auto mwit = mw_by_pair.find(pe.key);
        const bool mw_flagged = (mwit != mw_by_pair.end());
        const bool gf_flagged = pe.gf_exceeds && arc.cooldown_remaining == 0;
        const bool flagged    = mw_flagged || gf_flagged;

        // --- Late across-gap re-route -----------------------------------
        // The chain already has its own TD2 (row not bridged) but the pre-gap
        // stash is still valid, and either (a) MW, whose locked-arc TD
        // confirmation lags the chain rebuild by one or more epochs, has only
        // now delivered its across-gap flag, or (b) a WL-settle deferral is
        // pending on this pair and is re-evaluated.  The rebuilt chain's TD2
        // cannot see the across-gap jump by construction, so a flat TD2 here
        // does not affirm GF stability; evaluated as-is, the MW-only veto
        // would classify a genuine re-lock slip (for example −28·λ₂ = 6.84 m
        // across a 2-epoch dropout) as a code event.  The row is re-routed to
        // the drift-corrected across-gap jump and the standard bridged
        // adjudication runs.  This applies only when the chain's own TD2 does
        // not exceed its gate: a same-epoch slip the chain can see is
        // adjudicated by the chain.  For a pending deferral, such a new event
        // also aborts the wait (the level reference is no longer the only
        // unknown), and the satellite is demoted to the conservative reset.
        if (!pe.bridged && (mw_flagged || arc.wl_settle_pending) &&
            arc.bridge_valid) {
            const double span_s    = epoch_now_s - arc.bridge_epoch_s;
            const double stash_cap = arc.wl_settle_pending
                ? bridge_max_gap_s_ + wl_settle_max_s_
                : bridge_max_gap_s_;
            if (pe.gf_exceeds && arc.wl_settle_pending) {
                log->warn("TurboEdit WL-settle deferral ABORTED for {} {}/{}: "
                          "the post-gap GF chain flagged a new event while "
                          "waiting — conservative reset",
                          sat, pe.sig1, pe.sig2);
                out.reset_sats.insert(sat);
                arc.bridge_valid      = false;
                arc.wl_settle_pending = false;
                held_across_lli_.erase(sat + "_" + pe.sig1);
                held_across_lli_.erase(sat + "_" + pe.sig2);
            } else if (!pe.gf_exceeds && span_s > 0.0 && span_s <= stash_cap) {
                const double drift = std::isfinite(arc.bridge_rate_mps)
                                         ? arc.bridge_rate_mps * span_s : 0.0;
                pe.td2     = pe.gf - arc.bridge_gf - drift;
                pe.gap_s   = span_s;
                pe.bridged = true;
                if (mw_flagged && !arc.wl_settle_pending)
                    log->info("TurboEdit late across-gap adjudication {} {}/{}: "
                              "MW flag arrived after the GF chain rebuilt "
                              "(TD-confirm lag); re-evaluating against the "
                              "pre-gap level (dGF={:+.4f} m over {:.1f} s)",
                              sat, pe.sig1, pe.sig2, pe.td2, span_s);
            } else if (!pe.gf_exceeds) {
                arc.bridge_valid      = false;
                arc.wl_settle_pending = false;
            }
        }

        bool did_slip = false;        // treat as discontinuity → reset arc + cooldown
        int  dN1 = 0, dN2 = 0;        // for trace
        double frac2 = 0.0;           // for trace
        int  dN_wl_trace = 0;
        bool wl_known_trace = true;
        const char* action = "none";

        // Per-(sat, signal) integer staging, reconciled across the sat's
        // pairs (a disagreement on a shared signal demotes the whole sat
        // to a reset).  Shared by the joint-solve repair branch, the
        // code-event veto (affirmative zeros), and the gap-bridge dispatch.
        auto stage = [&](const std::string& sig, int dN, double lam, double fr) {
            auto& sigmap = cand[sat];
            auto it = sigmap.find(sig);
            if (it == sigmap.end()) {
                sigmap[sig] = dN;
                cand_lambda[sat][sig] = lam;
                cand_frac[sat][sig]   = fr;
            } else if (it->second != dN) {
                conflict_sats.insert(sat);  // disagreement across pairs
            }
        };

        if (pe.bridged) {
            // === Gap-bridge adjudication (see the header's bridge note) ===
            // Only MW flags (or an in-progress WL-settle deferral) are
            // adjudicated across a gap.  A GF-only bridged exceedance has no
            // affirmable MW-clean evidence across a re-acquisition and no
            // mandate to suppress, so it stays under the KF innovation
            // backstop, as it would without the bridge.  An untrusted or
            // uninformed bridge likewise leaves the MW mandate to the caller,
            // the same outcome as without the bridge.
            if (mw_flagged || arc.wl_settle_pending) {
                if (mw_flagged) {
                    dN_wl_trace    = mwit->second.known ? mwit->second.dN_wl : 0;
                    wl_known_trace = mwit->second.known;
                }
                bool actioned  = false;
                const bool trusted =
                    iono_stable_cons &&
                    !pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2) &&
                    arc.diff_window.size() >= kCodeEventMinMadSamples &&
                    std::abs(pe.lambda1 - pe.lambda2) >= 1e-6;
                if (trusted) {
                    const double denom = pe.lambda1 - pe.lambda2;
                    const double mad_sigma = computeMadSigma(arc.diff_window);
                    const double widen =
                        (arc.bridge_dt0_s > 0.0 && pe.gap_s > arc.bridge_dt0_s)
                            ? std::sqrt(pe.gap_s / arc.bridge_dt0_s) : 1.0;
                    const double zero_gate =
                        kCodeEventZeroSigma * mad_sigma * widen;
                    // Rate-adaptive evidence over the gap: the σ the split
                    // rounds against is the arc's own, extrapolated by the
                    // same random-walk factor as the gates.  The zero
                    // affirmation also requires lattice discrimination, with
                    // a cap that is not widened, because the lattice does not
                    // stretch with the gap.
                    const double sigma_bridge = mad_sigma * widen;
                    sigma_trace = sigma_bridge;
                    const double zero_cap =
                        kVetoZeroGateMaxLambdaFrac * std::abs(denom);
                    if (mw_only_veto_ && mw_flagged && !arc.wl_settle_pending &&
                        std::isfinite(zero_gate) &&
                        zero_gate > 0.0 && zero_gate <= zero_cap &&
                        std::abs(pe.td2) <= zero_gate) {
                        // Flat across the gap: the GF level did not move, so
                        // the MW step is in the code observable (a
                        // re-acquisition DLL transient); a code event.
                        stage(pe.sig1, 0, pe.lambda1, 0.0);
                        stage(pe.sig2, 0, pe.lambda2, 0.0);
                        out.code_events.push_back({ sat, pe.sig1, pe.sig2,
                                                    dN_wl_trace,
                                                    mwit->second.known,
                                                    pe.td2, zero_gate });
                        action = "code_event";
                        log->info("TurboEdit bridged code-domain event {} {}/{}: "
                                  "MW flagged across a {:.1f} s gap but the GF "
                                  "level is unmoved (|dGF|={:.4f} m <= {:.4f} m) "
                                  "— phase preserved",
                                  sat, pe.sig1, pe.sig2, pe.gap_s,
                                  pe.td2, zero_gate);
                        actioned = true;
                    } else if (!splitCapable(sigma_bridge, denom)) {
                        // Across-gap evidence too coarse to attribute
                        // per-signal integers (the gap-widened σ exceeds the
                        // split gate: coarse rate, long span, or a noisy
                        // arc).  A pending WL-settle deferral cannot resolve
                        // against a gate that only widens with the wait, so
                        // it aborts to the conservative reset.  Otherwise the
                        // resolver takes no action and the caller's MW
                        // mandate fires, as it would without the bridge.
                        if (arc.wl_settle_pending) {
                            log->warn("TurboEdit WL-settle deferral ABORTED "
                                      "for {} {}/{}: bridged GF σ={:.4f} m "
                                      "exceeds the split gate {:.4f} m — "
                                      "conservative reset",
                                      sat, pe.sig1, pe.sig2, sigma_bridge,
                                      split_max_sigma_ * std::abs(denom));
                            out.reset_sats.insert(sat);
                            did_slip = true;
                            action = "reset";
                            actioned = true;
                        } else {
                            log->info("TurboEdit bridged adjudication {} "
                                      "{}/{} declined: GF σ={:.4f} m over a "
                                      "{:.1f} s gap exceeds the split gate "
                                      "{:.4f} m — MW mandate left to the "
                                      "caller",
                                      sat, pe.sig1, pe.sig2, sigma_bridge,
                                      pe.gap_s,
                                      split_max_sigma_ * std::abs(denom));
                            action = "coarse_gf";
                        }
                    } else {
                        // Across-gap attribution.  WL candidate sources, in
                        // order of trust: (1) the published MW integer; (2) at
                        // the flag epoch, round(float) ± kBridgeKWindow (3)
                        // from the instantaneous re-acquisition float, which
                        // NL code bias can offset by several cycles (the wide
                        // window admits that bias instead of letting it make
                        // a wrong candidate unique within a narrow window);
                        // (3) on deferral epochs, the settled across-gap
                        // estimate (post-restart MW Hatch mean minus the
                        // stashed pre-jump level), used only once it is
                        // confidently roundable, and taken as its rounded
                        // integer alone (no window; see below).  The GF
                        // equation alone is degenerate along the null-line
                        // lattice ((0,−3) and (9,4) fit +0.733 and +0.736 m
                        // for GPS L1/L2; only k separates them), so a unique
                        // candidate is required after the fractional,
                        // GF-separation and sibling-affirmation gates.
                        std::vector<int> k_cands;
                        bool k_published = false;
                        bool k_settled   = false;
                        if (mw_flagged && mwit->second.known) {
                            k_cands.push_back(mwit->second.dN_wl);
                            k_published = true;
                        } else if (arc.wl_settle_pending) {
                            // Deferral epoch: only the settled across-gap
                            // estimate is consulted.  It is confidently
                            // roundable by the settle criterion, so it selects
                            // its rounded integer directly; a window around it
                            // would re-admit the lattice ambiguity that the
                            // wait resolves.
                            const auto evit = wl_gap_evidence.find(pe.key);
                            if (evit != wl_gap_evidence.end() &&
                                evit->second.stable) {
                                k_cands.push_back(static_cast<int>(
                                    std::llround(evit->second.float_cycles)));
                                k_settled = true;
                                log->info("TurboEdit WL-settle for {} {}/{}: "
                                          "across-gap WL settled at {:+.2f} "
                                          "cycles (σ_mean={:.3f}, n={}) — "
                                          "re-running the bridged solve with "
                                          "k={:+d}",
                                          sat, pe.sig1, pe.sig2,
                                          evit->second.float_cycles,
                                          evit->second.sigma_mean_cycles,
                                          evit->second.post_count,
                                          k_cands.back());
                            }
                        } else if (mw_flagged &&
                                   std::isfinite(mwit->second.wl_float)) {
                            // Flag epoch: the instantaneous re-acquisition
                            // float, admitted with a wide window for its
                            // NL-code bias.
                            const int r = static_cast<int>(
                                std::llround(mwit->second.wl_float));
                            for (int kk = r - kBridgeKWindow;
                                 kk <= r + kBridgeKWindow; ++kk)
                                k_cands.push_back(kk);
                        }
                        // Sibling-pair affirmation: signals of this sat's
                        // continuous, unflagged cross-frequency arcs are
                        // affirmed unslipped; candidates that require a
                        // nonzero integer on an affirmed signal are pruned.
                        const std::set<std::string> affirmed =
                            affirmedZeroSignals(sat, pe.key, arc.bridge_epoch_s);
                        int    best_k = 0, best_dN2 = 0;
                        double best_frac = 1.0;
                        int    n_acceptable = 0;
                        int    n_sibling_pruned = 0;
                        for (const int k : k_cands) {
                            const double dN2_f =
                                (pe.td2 - pe.lambda1 * static_cast<double>(k)) / denom;
                            const int    d2 = static_cast<int>(std::llround(dN2_f));
                            const double fr = std::abs(dN2_f - static_cast<double>(d2));
                            const int    d1 = d2 + k;
                            const bool sane = std::abs(d1) <= kMaxSlipCycles &&
                                              std::abs(d2) <= kMaxSlipCycles;
                            // GF-separation guard: the candidate's predicted
                            // jump must exceed the zero gate.  Otherwise a
                            // marginal noise exceedance of the zero gate could
                            // be attributed to a null-line pair ((9,7) predicts
                            // 3.2 mm for GPS L1/L2) and produce a 9-cycle
                            // repair on a continuous signal from noise alone.
                            // A candidate set with no separable member falls
                            // through to the conservative reset.
                            const bool gf_separated =
                                std::abs(pe.lambda1 * static_cast<double>(d1) -
                                         pe.lambda2 * static_cast<double>(d2)) > zero_gate;
                            if (!(fr <= kMaxFractional && sane &&
                                  (d1 != 0 || d2 != 0) && gf_separated))
                                continue;
                            if ((d1 != 0 && affirmed.count(pe.sig1)) ||
                                (d2 != 0 && affirmed.count(pe.sig2))) {
                                ++n_sibling_pruned;
                                continue;
                            }
                            ++n_acceptable;
                            if (fr < best_frac) {
                                best_frac = fr; best_k = k; best_dN2 = d2;
                            }
                        }
                        if (n_sibling_pruned > 0) {
                            std::string aff_txt;
                            for (const auto& s : affirmed)
                                aff_txt += (aff_txt.empty() ? "" : ",") + s;
                            log->info("TurboEdit bridged solve {} {}/{}: "
                                      "sibling-pair affirmation pruned {} "
                                      "GF-consistent candidate(s) (affirmed-"
                                      "unslipped signal(s): {})",
                                      sat, pe.sig1, pe.sig2, n_sibling_pruned,
                                      aff_txt);
                        }
                        if (n_acceptable == 1) {
                            dN2 = best_dN2;
                            dN1 = best_dN2 + best_k;
                            frac2 = best_frac;
                            dN_wl_trace = best_k;
                            // Dispatch per signal by observation-domain
                            // continuity: a signal observed through every gap
                            // epoch kept its ambiguity and gets an integer
                            // repair; a signal that missed an epoch was pruned
                            // and reseeded, so its slip is moot.
                            bool any_repair = false;
                            auto dispatch = [&](const std::string& sig, int dN,
                                                double lam) {
                                stage(sig, dN, lam, frac2);
                                if (dN == 0) return;
                                const std::string sigkey = sat + "_" + sig;
                                // A signal the caller holds across an LLI gap
                                // counts as continuous: the hold preserved its
                                // pre-gap value, so the across-gap integer is a
                                // real repair, not an already-absorbed reseed.
                                // Other signals use observation-domain
                                // continuity.
                                const auto cs = sig_continuous_since_s_.find(sigkey);
                                const bool continuous =
                                    held_across_lli_.count(sigkey) > 0 ||
                                    (cs != sig_continuous_since_s_.end() &&
                                     cs->second <= arc.bridge_epoch_s + 1e-6);
                                if (continuous) any_repair = true;
                                else            cand_moot[sat].insert(sig);
                            };
                            dispatch(pe.sig1, dN1, pe.lambda1);
                            dispatch(pe.sig2, dN2, pe.lambda2);
                            action = any_repair ? "repair" : "moot";
                            log->info("TurboEdit bridged attribution {} {}/{}: "
                                      "dGF={:+.4f} m over {:.1f} s + k={:+d} → "
                                      "(dN1={:+d}, dN2={:+d}), frac={:.3f} — {}{}",
                                      sat, pe.sig1, pe.sig2, pe.td2, pe.gap_s,
                                      best_k, dN1, dN2, frac2,
                                      any_repair ? "repair (continuous signal)"
                                                 : "moot (reseeded during gap)",
                                      arc.wl_settle_pending
                                          ? " [WL-settle deferred adjudication]"
                                          : "");
                            actioned = true;
                        } else if (wl_settle_enabled_ && !k_published &&
                                   !k_settled) {
                            // Float-derived evidence is absent, not yet
                            // settled, or still ambiguous (n_acceptable ≥ 2:
                            // candidates separated by a null-line pair).  A
                            // guess can produce a confident wrong repair (for
                            // example (−9, −35) against a true (0, −28)), so
                            // the adjudication is deferred while the
                            // post-restart MW mean settles, bounded by the
                            // configured cap.  The caller keeps the holds
                            // active and suppresses the MW mandate via
                            // out.pending_sats.
                            if (!arc.wl_settle_pending) {
                                arc.wl_settle_pending = true;
                                arc.wl_settle_start_s = epoch_now_s;
                                log->info("TurboEdit WL-settle deferral START "
                                          "for {} {}/{}: dGF={:+.4f} m solid "
                                          "but the WL evidence is {} — waiting "
                                          "for the post-restart MW mean "
                                          "(cap {:.1f} s)",
                                          sat, pe.sig1, pe.sig2, pe.td2,
                                          n_acceptable >= 2
                                              ? "lattice-ambiguous"
                                              : "unsettled",
                                          wl_settle_max_s_);
                            }
                            if (epoch_now_s - arc.wl_settle_start_s >
                                wl_settle_max_s_) {
                                log->warn("TurboEdit WL-settle deferral TIMED "
                                          "OUT for {} {}/{} after {:.1f} s — "
                                          "conservative reset",
                                          sat, pe.sig1, pe.sig2,
                                          epoch_now_s - arc.wl_settle_start_s);
                                out.reset_sats.insert(sat);
                                did_slip = true;
                                action = "reset";
                                actioned = true;
                            } else {
                                out.pending_sats.insert(sat);
                                action = "defer";
                                // Not actioned: the stash and the held marks
                                // persist to the next epoch's re-evaluation.
                            }
                        } else if (!k_cands.empty()) {
                            // Active failure: the across-gap jump fits no
                            // candidate, or more than one, and deferral is
                            // unavailable; conservative reset.
                            out.reset_sats.insert(sat);
                            did_slip = true;
                            action = "reset";
                            actioned = true;
                        }
                        // k_cands empty (no deferral): no WL information at
                        // all.  The resolver takes no action and the caller's
                        // own mandate fires, as it would without the bridge.
                    }
                }
                // An untrusted epoch (IVI burst, LLI on the pair) while a
                // deferral is pending neither adjudicates nor abandons it: the
                // caller keeps suppressing until trust returns or the timeout
                // fires.
                if (!actioned && arc.wl_settle_pending) {
                    out.pending_sats.insert(sat);
                    action = "defer";
                }
                if (actioned) {
                    arc.bridge_valid      = false;  // one-shot
                    arc.wl_settle_pending = false;
                    // The held-across-LLI assertion is consumed by the same
                    // action that consumes the bridge: whatever the bridge
                    // decided (repair / reset / code event), the caller will
                    // release its hold this epoch.
                    held_across_lli_.erase(sat + "_" + pe.sig1);
                    held_across_lli_.erase(sat + "_" + pe.sig2);
                }
            }
        } else if (flagged) {
            int  dN_wl   = 0;
            bool wl_known = true;
            if (mw_flagged) {
                if (mwit->second.known) dN_wl = mwit->second.dN_wl;
                else                    wl_known = false;  // MW slip with no integer → can't split
            } else if (mw_clean_pairs.count(pe.key) == 0) {
                // GF-only flag without affirmative MW-clean evidence for the
                // pair this epoch.  MW silence does not prove ΔN_WL = 0: a
                // wl-locked arc absorbs a real slip into Welford without
                // emitting a result, and an immature (post-gap, cold-start)
                // arc cannot flag at all.  Forcing ΔN_WL = 0 in those states
                // aliases unequal slips into confident wrong dual-signal
                // repairs (GPS L1/L2: λ1/(λ1−λ2) ≈ −3.53 puts ΔN_WL = ±2, ±4,
                // ±6 at frac ≈ 0.06, 0.12, 0.18, inside kMaxFractional).  The
                // wide-lane is treated as unknown, which leads to the reset
                // fallback below.
                wl_known = false;
            }  // else GF-only with MW affirmatively clean → ΔN_WL = 0 holds
            dN_wl_trace = dN_wl;
            wl_known_trace = wl_known;

            const double denom = pe.lambda1 - pe.lambda2;

            // --- MW-only code-domain veto (GF-stable non-slip) -----------
            // MW flagged the pair but the GF jump is affirmatively zero: |TD2|
            // lies within the arc's own measured noise (kCodeEventZeroSigma ×
            // MAD, tighter than the detection gate and, on quiet arcs, tighter
            // than the kMinThresholdM quantum floor, so a genuine (4,3)-class
            // GF jump of ~2.9 cm for GPS L1/L2 still reaches the joint solve
            // below).  With the phase still, no plausible integer pair fits
            // the MW step (see the header's null-line geometry), and the
            // event is classified as a code-domain artifact: both signals are
            // adjudicated clean, the caller skips the phase reset, and the KF
            // phase innovation (standardized residual gate) backstops the
            // remaining null-line-pair risk.
            // Eligibility requires every trust gate the GF evidence rests on:
            // a zero gate that can discriminate (3.5σ at or below
            // kVetoZeroGateMaxLambdaFrac × |λ1−λ2|; at coarse rates the
            // un-cancelled iono residual inflates the MAD until "zero" cannot
            // be told apart from a small slip or a near-null-line pair, and
            // the veto then does not apply), an IVI-stable constellation, a
            // mature arc MAD (a warmup arc cannot affirm zero), no post-slip
            // cooldown echo, and no receiver-declared loss of lock on either
            // signal (an LLI corroborates the slip, and the caller's LLI
            // mandate governs).
            // wl_known is not required: a non-integer MW excursion with a
            // still phase is, if anything, more code-like.
            bool code_event = false;
            if (mw_only_veto_ && mw_flagged && !pe.gf_exceeds &&
                arc.cooldown_remaining == 0 && iono_stable_cons &&
                !pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2)) {
                // Affirming zero requires more MAD maturity than detection
                // (kCodeEventMinMadSamples > kMinEpochsForAdaptive): a 5-sample
                // MAD has too much estimator variance to support a
                // state-preserving decision.
                const double mad_sigma =
                    (arc.diff_window.size() >= kCodeEventMinMadSamples)
                        ? computeMadSigma(arc.diff_window)
                        : std::numeric_limits<double>::quiet_NaN();
                const double zero_gate = kCodeEventZeroSigma * mad_sigma;
                const double zero_cap =
                    kVetoZeroGateMaxLambdaFrac * std::abs(denom);
                if (std::isfinite(zero_gate) && zero_gate > 0.0 &&
                    zero_gate <= zero_cap &&
                    std::abs(pe.td2) <= zero_gate) {
                    code_event = true;
                    // Diagnostics only: the null-line pair the measured GF
                    // would imply, traced so the implausible magnitude
                    // ((9,7), (4,3), ...) is visible; it is never applied.
                    if (wl_known && std::abs(denom) >= 1e-6) {
                        const double dN2_f =
                            (pe.td2 - pe.lambda1 * static_cast<double>(dN_wl)) / denom;
                        dN2 = static_cast<int>(std::llround(dN2_f));
                        frac2 = std::abs(dN2_f - static_cast<double>(dN2));
                        dN1 = dN2 + dN_wl;
                    }
                    stage(pe.sig1, 0, pe.lambda1, frac2);
                    stage(pe.sig2, 0, pe.lambda2, frac2);
                    out.code_events.push_back({ sat, pe.sig1, pe.sig2,
                                                dN_wl, wl_known, pe.td2, zero_gate });
                    action = "code_event";
                    log->info("TurboEdit code-domain event {} {}/{}: MW flagged "
                              "(dN_WL={:+d}{}) but GF affirmatively stable "
                              "(|TD2|={:.4f} m <= {:.4f} m) — classified as code "
                              "artifact (implied null-line pair dN1={:+d}, dN2={:+d}); "
                              "phase preserved, KF residual gate is the backstop",
                              sat, pe.sig1, pe.sig2, dN_wl,
                              wl_known ? "" : ", integer unknown",
                              pe.td2, zero_gate, dN1, dN2);
                    // did_slip stays false: the GF chain did not jump.  It
                    // keeps rolling so the next epoch's veto (a persistent
                    // code excursion re-flags after the MW arc restarts at
                    // the new level) rests on live evidence, and the clean
                    // TD2 feeds the MAD window below as usual.
                }
            }

            const bool split_capable = splitCapable(sigma_trace, denom);

            if (code_event) {
                // Handled above.
            } else if (!mw_flagged && !split_capable) {
                // GF-only exceedance on an arc that is not split-capable (the
                // coarse-rate iono residual at lattice scale, or a noisy
                // arc): not attributable to per-signal integers and with no
                // MW mandate to honor, so it is ignored.  A reset here would
                // turn every coarse-rate iono transient into a reset that
                // discards converged ambiguities; a genuine equal slip, with
                // a phase-innovation signature of |dN·λ| ≥ 19 cm, is left to
                // the KF residual backstop.  The arc keeps rolling and the
                // exceeding TD2 stays out of the MAD window as usual.
                action = "coarse_gf";
                log->info("TurboEdit {} {}/{}: GF-only exceedance "
                          "(|TD2|={:.4f} m) on a split-incapable arc "
                          "(σ={:.4f} m > {:.2f}×|λ1−λ2|={:.4f} m) — ignored; "
                          "KF residual backstop governs",
                          sat, pe.sig1, pe.sig2, std::abs(pe.td2),
                          sigma_trace, split_max_sigma_, split_gate_m);
            } else if (!iono_stable_cons || !wl_known || std::abs(denom) < 1e-6) {
                // GF jump untrustworthy (iono) / no WL integer / degenerate pair → reset.
                out.reset_sats.insert(sat);
                did_slip = true;
                action = "reset";
            } else if (!split_capable) {
                // The MW mandate stands but the GF evidence cannot split it
                // into per-signal integers (rate-adaptive gate): conservative
                // reset, as without TurboEdit.
                out.reset_sats.insert(sat);
                did_slip = true;
                action = "reset";
                log->info("TurboEdit {} {}/{}: MW flagged (dN_WL={:+d}{}) but "
                          "GF noise too coarse for the per-signal split "
                          "(σ={:.4f} m > {:.2f}×|λ1−λ2|={:.4f} m) — reset "
                          "fallback",
                          sat, pe.sig1, pe.sig2, dN_wl,
                          wl_known ? "" : ", integer unknown",
                          sigma_trace, split_max_sigma_, split_gate_m);
            } else {
                const double dN2_f = (pe.td2 - pe.lambda1 * static_cast<double>(dN_wl)) / denom;
                dN2 = static_cast<int>(std::llround(dN2_f));
                frac2 = std::abs(dN2_f - static_cast<double>(dN2));
                dN1 = dN2 + dN_wl;
                const bool sane = std::abs(dN1) <= kMaxSlipCycles &&
                                  std::abs(dN2) <= kMaxSlipCycles;

                if (frac2 <= kMaxFractional && sane && (dN1 != 0 || dN2 != 0)) {
                    stage(pe.sig1, dN1, pe.lambda1, frac2);
                    stage(pe.sig2, dN2, pe.lambda2, frac2);
                    did_slip = true;
                    action = "repair";
                } else if (dN1 == 0 && dN2 == 0) {
                    // Resolved to "no slip" (e.g. GF-only false alarm); the arc keeps rolling.
                    action = "none";
                } else {
                    out.reset_sats.insert(sat);
                    did_slip = true;
                    action = "reset";
                }
            }
        }

        // Trace row.
        if (trace_file_.is_open()) {
            trace_file_ << std::fixed << std::setprecision(3)
                        << current_epoch_.week << "," << current_epoch_.tow << ","
                        << sat << "," << PppUtils::getSystemString(pe.obs->system) << ","
                        << std::setprecision(2) << pe.obs->elevation << ","
                        << pe.sig1 << "," << pe.sig2 << ","
                        << std::setprecision(6) << pe.lambda1 << "," << pe.lambda2 << ","
                        << pe.gf << "," << pe.td2 << ","
                        << (std::isfinite(pe.thresh) ? pe.thresh : -1.0) << ","
                        << std::setprecision(4) << ivi << "," << (iono_stable_cons ? 1 : 0) << ","
                        << (mw_flagged ? 1 : 0) << "," << (gf_flagged ? 1 : 0) << ","
                        << dN_wl_trace << "," << (wl_known_trace ? 1 : 0) << ","
                        << dN1 << "," << dN2 << ","
                        << std::setprecision(3) << frac2 << "," << action << ","
                        << std::setprecision(6)
                        << (std::isfinite(pe.thresh_unfloored) ? pe.thresh_unfloored
                                                               : -1.0)
                        << "," << (pe.bridged ? 1 : 0) << ","
                        // Rate-adaptive evidence: the σ the gates were
                        // scaled by (gap-widened on bridged rows; -1 = no
                        // estimate) and the split-capability bound.
                        << (std::isfinite(sigma_trace) ? sigma_trace : -1.0)
                        << "," << split_gate_m
                        << "\n";
        }

        // Arc update.
        if (did_slip) {
            // The chain re-seeds; the MAD window is preserved (the same
            // calibration-versus-chain split as the short-gap partial reset).
            // The level jump does not change the TD2 noise, and the window
            // holds only clean (non-exceeding) TD2s, so wiping it would cost
            // kMinEpochsForAdaptive (5) epochs without the arc's own threshold
            // after every slip, with no statistical reason.  With the window
            // intact, post-slip epochs re-arm at the arc's own calibration;
            // the cross-sat floor covers only genuinely fresh arcs.
            //
            // last_epoch_s is stamped (not zeroed) because the window
            // survives: it is the gap clock that lets the absolute-gap
            // trigger expire the preserved calibration.  If a satellite
            // slips, sets, and rises hours later, the stamped time makes
            // absolute_gap fire on return and wipe the arc.  Without it
            // (count==0 and last_epoch_s==0 disarm both gap triggers), an
            // hours-old MAD would govern the mature branch in a different
            // elevation and multipath regime.
            arc.prev_gf = 0.0; arc.prev_prev_gf = 0.0;
            arc.count = 0;
            arc.last_epoch_s = epoch_now_s;
            arc.cooldown_remaining = kCooldownEpochs;  // suppress the TD2 echo next epoch
        } else {
            // Only clean (non-outlier) TD2s enter the MAD window, so a
            // suppressed exceedance cannot inflate the threshold.  A bridged
            // value is an across-gap jump, not a TD2 sample, and is never
            // admitted.
            if (!pe.gf_exceeds && !pe.bridged) {
                arc.diff_window.push_back(pe.td2);
                if (arc.diff_window.size() > kGfWindowSize) arc.diff_window.pop_front();
            }
            const double ep_s = TimeUtils::continuousGpsSeconds(pe.obs->time);
            // GF-rate EWMA (gap-bridge drift correction), consecutive epochs
            // only.  Any chain break already re-seeded in pass 1, so an arc
            // that reaches this point with count >= 1 is consecutive.
            if (arc.count >= 1 && arc.last_epoch_s > 0.0 && ep_s > arc.last_epoch_s) {
                const double dt = ep_s - arc.last_epoch_s;
                const double rate = (pe.gf - arc.prev_gf) / dt;
                arc.rate_ewma_mps = std::isfinite(arc.rate_ewma_mps)
                    ? kBridgeRateAlpha * rate +
                          (1.0 - kBridgeRateAlpha) * arc.rate_ewma_mps
                    : rate;
                arc.last_spacing_s = dt;
            }
            ++arc.count;
            arc.prev_prev_gf = arc.prev_gf;
            arc.prev_gf      = pe.gf;
            arc.last_epoch_s = ep_s;
            if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
        }
    }

    // --- Reconcile candidates into repairs (per-sat atomic) ---
    for (const auto& s : conflict_sats) out.reset_sats.insert(s);

    for (const auto& [sat, sigmap] : cand) {
        if (out.reset_sats.count(sat)) continue;  // a failed/conflicting pair → reset whole sat
        const auto moot_it = cand_moot.find(sat);
        for (const auto& [sig, dN] : sigmap) {
            if (dN == 0) {
                // Adjudicated by a confident joint solve: this signal did
                // not slip.  Reported so the caller can exempt it from its
                // own (pair-granular) reset mandates.
                out.clean_signals[sat].insert(sig);
                continue;
            }
            if (moot_it != cand_moot.end() && moot_it->second.count(sig)) {
                // Bridged attribution on a signal that was reseeded during
                // the gap: the fresh seed already absorbs the slip, so the
                // mandate is suppressed and no state changes.
                out.moot_signals[sat].insert(sig);
                continue;
            }
            SignalRepair r;
            r.satellite = sat;
            r.signal    = sig;
            r.dN        = dN;
            r.lambda    = cand_lambda.at(sat).at(sig);
            r.frac      = cand_frac.at(sat).at(sig);
            out.repairs.push_back(std::move(r));
        }
    }

    // Withdraw code events for satellites that ended the epoch in
    // reset_sats (another pair required a reset, or a staged-integer
    // conflict demoted the sat).  Contradictory evidence resolves to the
    // conservative outcome atomically per satellite, as in the emission loop
    // above, which already suppresses such a sat's clean_signals.  Otherwise
    // the caller would skip the reset mandate this resolution just issued.
    if (!out.code_events.empty() && !out.reset_sats.empty()) {
        out.code_events.erase(
            std::remove_if(out.code_events.begin(), out.code_events.end(),
                           [&out](const CodeEvent& ce) {
                               return out.reset_sats.count(ce.satellite) > 0;
                           }),
            out.code_events.end());
    }

    // A satellite that another pair demoted to reset does not also ask the
    // caller to suppress its mandates: its pending adjudication is withdrawn.
    // The reset takes precedence; the arcs, and the pending flag with them,
    // are erased when the caller forwards resetSatellite.
    if (!out.pending_sats.empty()) {
        for (auto it = out.pending_sats.begin(); it != out.pending_sats.end(); )
            it = out.reset_sats.count(*it) ? out.pending_sats.erase(it)
                                           : std::next(it);
    }

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

    return out;
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

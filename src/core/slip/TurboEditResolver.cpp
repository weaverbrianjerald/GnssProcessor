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
}  // namespace

// =====================================================================
// Construction
// =====================================================================

TurboEditResolver::TurboEditResolver(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      ppp_utils_(log_manager) {
    log_manager_.getLogger("turbo_edit_resolver")->info(
        "TurboEditResolver initialized (MW+GF joint repair; GF-only k={:.1f}, "
        "floor={:.3f} m, min_epochs={}, frac_max={:.2f}, IVI_thresh={:.3f} m, "
        "high-rate limit={:.1f} s)",
        kMadSigmaMultiplier, kMinThresholdM, kMinEpochsForAdaptive, kMaxFractional,
        kIviThreshold, kHighRateMaxIntervalS);
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
                << "dN1,dN2,frac,action\n";

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

double TurboEditResolver::computeAdaptiveThreshold(const GfArc& arc) const {
    const double sigma = computeMadSigma(arc.diff_window);
    if (!std::isfinite(sigma)) return std::numeric_limits<double>::quiet_NaN();
    return std::max(kMadSigmaMultiplier * sigma, kMinThresholdM);
}

double TurboEditResolver::computeIvi(const std::vector<double>& constellation_td2) {
    if (constellation_td2.size() < 3) return 0.0;
    std::vector<double> abs_vals;
    abs_vals.reserve(constellation_td2.size());
    for (const double v : constellation_td2) abs_vals.push_back(std::abs(v));
    return RobustStats::median(abs_vals) + RobustStats::madSigma(abs_vals);
}

// =====================================================================
// resolve() — two-pass joint solve
// =====================================================================

TurboEditResolver::EpochResolution TurboEditResolver::resolve(
    const std::vector<MwSlipResult>& mw_results,
    const std::vector<PppObservation>& observations,
    double interval_s) {

    auto log = log_manager_.getLogger("turbo_edit_resolver");
    EpochResolution out;

    // --- High-rate guard (warn once; gate still applies per-epoch via IVI) ---
    if (interval_s > 0.0 && interval_s > kHighRateMaxIntervalS) {
        out.rate_ok = false;
        if (!rate_warned_) {
            log->warn("TurboEdit: sampling interval {:.3f} s exceeds the high-rate "
                      "limit {:.1f} s — the geometry-free jump is iono-contaminated "
                      "at this rate; per-epoch IVI gating still applies and repairs "
                      "will frequently fall back to reset.",
                      interval_s, kHighRateMaxIntervalS);
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
    struct MwFlag { int dN_wl{ 0 }; bool known{ false }; };
    std::map<std::string, MwFlag> mw_by_pair;
    for (const auto& sr : mw_results) {
        if (sr.sig1.empty() || sr.sig2.empty()) continue;  // LLI / signal-less: not a pair slip
        const std::string sat = satOf(sr);
        if (sat.empty()) continue;
        auto& f = mw_by_pair[arcKey(sat, sr.sig1, sr.sig2)];
        // Prefer any entry that carries a usable integer for this pair.
        if (sr.slip_wl_known && !f.known) { f.dN_wl = sr.slip_wl_cycles; f.known = true; }
    }

    // --- Pass 1: GF + TD2 for every dual-band phase pair; collect IVI pools ---
    struct PairEpoch {
        const PppObservation* obs;
        std::string key, sig1, sig2;
        double gf{ 0.0 }, td2{ 0.0 }, lambda1{ 0.0 }, lambda2{ 0.0 };
        double thresh{ std::numeric_limits<double>::quiet_NaN() };
        bool   gf_exceeds{ false };  // |TD2| > thresh (outlier, ignoring cooldown)
    };
    std::vector<PairEpoch> cache;
    std::map<char, std::vector<double>> cons_td2;

    for (const auto& obs : observations) {
        if (obs.satellite.empty()) continue;
        if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
            continue;
        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

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
            const bool absolute_gap =
                arc.count > 0 && arc.last_epoch_s > 0.0 &&
                (epoch_s - arc.last_epoch_s) > max_gap_seconds_;
            const bool skipped_prev_epoch =
                arc.count > 0 && arc.last_epoch_s > 0.0 && prev_epoch_s_ > 0.0 &&
                arc.last_epoch_s < prev_epoch_s_ - 1e-6;
            if (absolute_gap || skipped_prev_epoch) {
                arc = GfArc{};
            }

            // Warm-up epochs (count < 2): roll forward here; no TD2 yet.
            if (arc.count < 2) {
                if (arc.count == 0) arc.prev_gf = gf;
                else { arc.prev_prev_gf = arc.prev_gf; arc.prev_gf = gf; }
                ++arc.count;
                arc.last_epoch_s = epoch_s;
                if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
                continue;
            }

            const double td2    = gf - 2.0 * arc.prev_gf + arc.prev_prev_gf;
            const double thresh = computeAdaptiveThreshold(arc);
            const bool gf_exceeds = std::isfinite(thresh) && std::abs(td2) > thresh;

            cache.push_back({ &obs, key, pr.phase1, pr.phase2, gf, td2, lam1, lam2,
                              thresh, gf_exceeds });
            cons_td2[obs.satellite[0]].push_back(td2);
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

    // --- Pass 2: flag (MW ∪ GF-only), joint solve, update arc ---
    // Per-(sat, signal) integer candidates, reconciled across a sat's pairs.
    std::map<std::string, std::map<std::string, int>>    cand;
    std::map<std::string, std::map<std::string, double>> cand_lambda;
    std::map<std::string, std::map<std::string, double>> cand_frac;
    std::set<std::string> conflict_sats;

    for (const auto& pe : cache) {
        auto& arc = gf_arcs_[pe.key];
        const std::string& sat = pe.obs->satellite;
        const char cons = sat[0];
        const double ivi = ivi_by_cons.count(cons) ? ivi_by_cons.at(cons) : 0.0;
        const bool iono_stable_cons = (ivi < kIviThreshold);

        auto mwit = mw_by_pair.find(pe.key);
        const bool mw_flagged = (mwit != mw_by_pair.end());
        const bool gf_flagged = pe.gf_exceeds && arc.cooldown_remaining == 0;
        const bool flagged    = mw_flagged || gf_flagged;

        bool did_slip = false;        // treat as discontinuity → reset arc + cooldown
        int  dN1 = 0, dN2 = 0;        // for trace
        double frac2 = 0.0;           // for trace
        int  dN_wl_trace = 0;
        bool wl_known_trace = true;
        const char* action = "none";

        if (flagged) {
            int  dN_wl   = 0;
            bool wl_known = true;
            if (mw_flagged) {
                if (mwit->second.known) dN_wl = mwit->second.dN_wl;
                else                    wl_known = false;  // MW slip with no integer → can't split
            }  // else GF-only → ΔN_WL = 0 (equal-slip assumption)
            dN_wl_trace = dN_wl;
            wl_known_trace = wl_known;

            const double denom = pe.lambda1 - pe.lambda2;

            if (!iono_stable_cons || !wl_known || std::abs(denom) < 1e-6) {
                // GF jump untrustworthy (iono) / no WL integer / degenerate pair → reset.
                out.reset_sats.insert(sat);
                did_slip = true;
                action = "reset";
            } else {
                const double dN2_f = (pe.td2 - pe.lambda1 * static_cast<double>(dN_wl)) / denom;
                dN2 = static_cast<int>(std::llround(dN2_f));
                frac2 = std::abs(dN2_f - static_cast<double>(dN2));
                dN1 = dN2 + dN_wl;
                const bool sane = std::abs(dN1) <= kMaxSlipCycles &&
                                  std::abs(dN2) <= kMaxSlipCycles;

                if (frac2 <= kMaxFractional && sane && (dN1 != 0 || dN2 != 0)) {
                    auto stage = [&](const std::string& sig, int dN, double lam) {
                        auto& sigmap = cand[sat];
                        auto it = sigmap.find(sig);
                        if (it == sigmap.end()) {
                            sigmap[sig] = dN;
                            cand_lambda[sat][sig] = lam;
                            cand_frac[sat][sig]   = frac2;
                        } else if (it->second != dN) {
                            conflict_sats.insert(sat);  // disagreement across pairs
                        }
                    };
                    stage(pe.sig1, dN1, pe.lambda1);
                    stage(pe.sig2, dN2, pe.lambda2);
                    did_slip = true;
                    action = "repair";
                } else if (dN1 == 0 && dN2 == 0) {
                    // Resolved to "no slip" (e.g. GF-only false alarm) — leave arc rolling.
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
                        << std::setprecision(3) << frac2 << "," << action << "\n";
        }

        // Arc update.
        if (did_slip) {
            arc.prev_gf = 0.0; arc.prev_prev_gf = 0.0; arc.last_epoch_s = 0.0;
            arc.count = 0; arc.diff_window.clear();
            arc.cooldown_remaining = kCooldownEpochs;  // suppress the TD2 echo next epoch
        } else {
            // Only feed clean (non-outlier) TD2 into the MAD window so a
            // suppressed exceedance can't inflate the threshold.
            if (!pe.gf_exceeds) {
                arc.diff_window.push_back(pe.td2);
                if (arc.diff_window.size() > kGfWindowSize) arc.diff_window.pop_front();
            }
            ++arc.count;
            arc.prev_prev_gf = arc.prev_gf;
            arc.prev_gf      = pe.gf;
            arc.last_epoch_s = TimeUtils::continuousGpsSeconds(pe.obs->time);
            if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
        }
    }

    // --- Reconcile candidates into repairs (per-sat atomic) ---
    for (const auto& s : conflict_sats) out.reset_sats.insert(s);

    for (const auto& [sat, sigmap] : cand) {
        if (out.reset_sats.count(sat)) continue;  // a failed/conflicting pair → reset whole sat
        for (const auto& [sig, dN] : sigmap) {
            if (dN == 0) continue;  // signal didn't slip
            SignalRepair r;
            r.satellite = sat;
            r.signal    = sig;
            r.dN        = dN;
            r.lambda    = cand_lambda.at(sat).at(sig);
            r.frac      = cand_frac.at(sat).at(sig);
            out.repairs.push_back(std::move(r));
        }
    }

    if (!out.repairs.empty() || !out.reset_sats.empty()) {
        log->info("TurboEdit epoch: {} per-signal repair(s), {} reset(s), "
                  "iono_stable={}, max_IVI={:.4f} m",
                  out.repairs.size(), out.reset_sats.size(),
                  out.iono_stable ? "yes" : "no", out.max_ivi);
    }

    // Advance the previous-epoch marker so the next call can detect per-signal
    // dropouts.  Only when this epoch actually carried observations (an empty
    // epoch leaves the marker so a returning arc is still seen as a gap).
    if (epoch_now_s > 0.0) prev_epoch_s_ = epoch_now_s;

    return out;
}

// =====================================================================
// External reset
// =====================================================================

void TurboEditResolver::resetSatellite(const std::string& satellite) {
    const std::string prefix = satellite + "_";
    for (auto it = gf_arcs_.begin(); it != gf_arcs_.end(); ) {
        if (it->first.rfind(prefix, 0) == 0) it = gf_arcs_.erase(it);
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
}

void TurboEditResolver::resetAll() {
    gf_arcs_.clear();
    prev_epoch_s_ = 0.0;
}

} // namespace GnssProcessor

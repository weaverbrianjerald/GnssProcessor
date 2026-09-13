/*!
 * \file IfPhaseSlipDetector.cpp
 * \brief Between-satellite, between-epoch IF phase slip detector.
 *
 * Organization:
 *  - Construction / tracing
 *  - Signal helpers
 *  - Statistics (median, MAD outlier check)
 *  - Observation grouping / reference selection
 *  - Double-difference processing
 *  - Top-level detect()
 *  - External reset
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include "common/Constants.hpp"
#include "math/RobustStats.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>

namespace GnssProcessor {

    // =================================================================
    // Construction / tracing
    // =================================================================

    IfPhaseSlipDetector::IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("if_slip_detector")->info(
            "IfPhaseSlipDetector initialized: ref_elev_thresh={:.0f} deg, outlier_sigma={:.1f}, pos_sigma_gate={:.2f} m",
            kRefElevThreshDeg, kOutlierSigmaMultiplier, kPositionSigmaThresholdM);
    }

    bool IfPhaseSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("if_slip_detector");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "if_slip_trace.csv";
        trace_file_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file_.is_open()) {
            logger->warn("Could not open IF slip trace file '{}'", trace_path.string());
            return false;
        }

        trace_file_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "phase_if_key,phi_if_omc_m,"
                    << "grad_phi_if_m,delta_grad_m,"
                    << "ref_satellite,ref_changed,"
                    << "lambda_if_m,is_slip,slip_reason,"
                    << "pos_sigma_3d_m,pos_gated\n";

        logger->info("IF slip tracing enabled: {}", trace_path.string());
        return true;
    }

    void IfPhaseSlipDetector::writeTraceRecord(const SatData& sd,
        const std::string& sys_str,
        double grad, double delta_grad,
        const std::string& ref_sat, bool ref_changed,
        double lambda_if, bool is_slip,
        const std::string& slip_reason,
        double position_sigma_3d, bool pos_gated) const {

        auto logger = log_manager_.getLogger("if_slip_detector");

        if (is_slip) {
            logger->info("[W{} {:.3f}] {} {} elev={:.1f} grad={:.6f} delta_grad={:.6f} "
                "ref={} lambda_if={:.6f} slip_reason={} pos_sigma={:.3f}{}",
                current_epoch_.week, current_epoch_.tow,
                sd.satellite, sys_str, sd.elevation,
                grad, delta_grad, ref_sat, lambda_if, slip_reason,
                position_sigma_3d, pos_gated ? " [pos_gated]" : "");
        } else {
            logger->debug("[W{} {:.3f}] {} {} elev={:.1f} grad={:.6f} delta_grad={:.6f} "
                "ref={}{} lambda_if={:.6f} reason={} pos_sigma={:.3f}{}",
                current_epoch_.week, current_epoch_.tow,
                sd.satellite, sys_str, sd.elevation,
                grad, delta_grad, ref_sat,
                ref_changed ? " [changed]" : "", lambda_if, slip_reason,
                position_sigma_3d, pos_gated ? " [pos_gated]" : "");
        }

        if (!trace_file_.is_open()) return;

        trace_file_ << std::fixed << std::setprecision(3)
            << current_epoch_.week << "," << current_epoch_.tow << ","
            << sd.satellite << "," << sys_str << ","
            << std::setprecision(2) << sd.elevation << ","
            << sd.phase_if_key << ","
            << std::setprecision(6) << sd.phi_if_omc << ","
            << grad << "," << delta_grad << ","
            << ref_sat << "," << (ref_changed ? 1 : 0) << ","
            << lambda_if << "," << (is_slip ? 1 : 0) << "," << slip_reason << ","
            << std::setprecision(3) << position_sigma_3d << "," << (pos_gated ? 1 : 0) << "\n";
    }

    // =================================================================
    // Signal helpers
    // =================================================================

    bool IfPhaseSlipDetector::findPhaseIfLabel(const PppObservation& obs, std::string& phase_if_key) const {
        phase_if_key.clear();
        for (const auto& [k, v] : obs.observed)
            if (k.find('_') != std::string::npos && !k.empty() && k[0] == 'L') {
                phase_if_key = k;
                return true;
            }
        return false;
    }

    bool IfPhaseSlipDetector::splitIfLabel(const std::string& label,
        std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    double IfPhaseSlipDetector::computeIfWavelengthMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2) const {

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return 0.0;

        const double denom = f1 * f1 - f2 * f2;
        if (std::abs(denom) < 1e-12) return 0.0;

        const double c1 = (f1 * f1) / denom;
        const double c2 = (f2 * f2) / denom;
        const double lambda_if = std::abs(c1 * Constants::SPEED_OF_LIGHT / f1
                                        - c2 * Constants::SPEED_OF_LIGHT / f2);
        return (std::isfinite(lambda_if) && lambda_if > 0.0) ? lambda_if : 0.0;
    }

    // =================================================================
    // Statistics (median, MAD outlier check)
    // =================================================================

    double IfPhaseSlipDetector::computeMedian(std::vector<double>& values) {
        // Delegates to the shared RobustStats::median.  This no longer sorts
        // `values` in place; every call site uses only the return value
        // (verified), so the dropped side effect is immaterial.
        return RobustStats::median(values);
    }

    std::map<std::string, double> IfPhaseSlipDetector::checkOutliers(
        const std::map<std::string, double>& values,
        const std::string& ref_satellite,
        double lambda_if) const {

        std::map<std::string, double> result = values;

        for (size_t pass = 0; pass < kMaxOutlierPasses; ++pass) {
            // Collect non-ref, finite values
            std::vector<double> non_ref_vals;
            for (const auto& [sat, v] : result)
                if (sat != ref_satellite && std::isfinite(v))
                    non_ref_vals.push_back(v);
            if (non_ref_vals.empty()) break;

            // MAD-based adaptive threshold
            const double median_val = computeMedian(non_ref_vals);
            std::vector<double> abs_devs(non_ref_vals.size());
            for (size_t i = 0; i < non_ref_vals.size(); ++i)
                abs_devs[i] = std::abs(non_ref_vals[i] - median_val);
            const double mad = computeMedian(abs_devs);
            const double thres_std = kOutlierSigmaMultiplier * 1.4826 * mad;

            // Max absolute value across all entries
            double max_abs_val = 0.0;
            for (const auto& [sat, v] : result)
                if (std::isfinite(v))
                    max_abs_val = std::max(max_abs_val, std::abs(v));

            if (thres_std <= lambda_if / 4.0 && max_abs_val <= 0.5 * lambda_if)
                break;

            // Remove worst outlier exceeding 0.5 * lambda_if
            double worst_val = 0.0;
            std::string worst_sat;
            for (const auto& [sat, v] : result) {
                if (sat == ref_satellite || !std::isfinite(v)) continue;
                if ((std::abs(v) > thres_std || std::abs(v) > lambda_if) &&
                    std::abs(v) > worst_val) {
                    worst_val = std::abs(v);
                    worst_sat = sat;
                }
            }

            if (worst_val > 0.5 * lambda_if && !worst_sat.empty())
                result[worst_sat] = std::numeric_limits<double>::quiet_NaN();
            else
                break;
        }

        return result;
    }

    // =================================================================
    // Observation grouping / reference selection
    // =================================================================

    std::map<std::string, std::vector<IfPhaseSlipDetector::SatData>>
    IfPhaseSlipDetector::groupByConstellation(const std::vector<PppObservation>& observations) const {

        std::map<std::string, std::vector<SatData>> result;

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            std::string phase_if_key;
            if (findPhaseIfLabel(obs, phase_if_key)) {
                // --- Combined (PPP) path: IF key already exists ---
                std::string sig1, sig2;
                if (!splitIfLabel(phase_if_key, sig1, sig2)) continue;

                auto obs_it = obs.observed.find(phase_if_key);
                auto mod_it = obs.modeled.find(phase_if_key);
                if (obs_it == obs.observed.end() || mod_it == obs.modeled.end()) continue;

                const double phi_if_omc = obs_it->second - mod_it->second;
                if (!std::isfinite(phi_if_omc)) continue;

                const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
                if (lambda_if <= 0.0) continue;

                result[PppUtils::getSystemString(obs.system)].push_back(
                    { obs.satellite, phase_if_key, sig1, sig2,
                      phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
            } else {
                // --- Uncombined (PPP-RTK) path: synthesize IF OMC ---
                // Find two phase signals on different frequency bands.
                std::vector<std::tuple<std::string, double, double>> phase_signals; // sig, observed, modeled
                for (const auto& [key, val] : obs.observed) {
                    if (key.empty() || key[0] != 'L') continue;
                    if (key.find('_') != std::string::npos) continue; // skip IF combinations
                    auto mod_it = obs.modeled.find(key);
                    if (mod_it == obs.modeled.end()) continue;
                    phase_signals.emplace_back(key, val, mod_it->second);
                }
                if (phase_signals.size() < 2) continue;

                // §14.10 PAIR WIDTH.  Off, exactly the pre-knob behaviour: the
                // FIRST two phase signals form the one IF combination, filed
                // under the bare system string.  On, every distinct pair is
                // formed; the first pair keeps that same chain (so its
                // population, its reference election, its double differences and
                // its log lines are unchanged) and each additional pair gets a
                // chain of its own, "<system>|<sig1>_<sig2>" — its own reference,
                // its own gradient history, its own reset key.
                const std::string sys_key = PppUtils::getSystemString(obs.system);
                const std::size_t n_use =
                    all_pairs_ ? phase_signals.size() : std::size_t{ 2 };
                for (std::size_t a = 0; a < n_use; ++a) {
                for (std::size_t b = a + 1; b < n_use; ++b) {
                const auto& [sig1, obs1, mod1] = phase_signals[a];
                const auto& [sig2, obs2, mod2] = phase_signals[b];

                const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
                const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
                if (!std::isfinite(f1) || !std::isfinite(f2) || f1 <= 0.0 || f2 <= 0.0) continue;

                const double f1sq = f1 * f1;
                const double f2sq = f2 * f2;
                const double denom = f1sq - f2sq;
                if (std::abs(denom) < 1e-12) continue;

                const double c1 = f1sq / denom;
                const double c2 = -f2sq / denom;

                const double omc1 = obs1 - mod1;
                const double omc2 = obs2 - mod2;
                const double phi_if_omc = c1 * omc1 + c2 * omc2;
                if (!std::isfinite(phi_if_omc)) continue;

                const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
                if (lambda_if <= 0.0) continue;

                const std::string synth_key = sig1 + "_" + sig2;
                const bool is_first_pair = (a == 0 && b == 1);

                result[is_first_pair ? sys_key : (sys_key + "|" + synth_key)].push_back(
                    { obs.satellite, synth_key, sig1, sig2,
                      phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
                }  // b — second band of the pair
                }  // a — first band of the pair
            }
        }

        return result;
    }

    bool IfPhaseSlipDetector::selectReference(const std::vector<SatData>& sat_vec,
        ConstellationState& cs) const {

        // Check if current reference is still present and above threshold.
        // Card R: an escalation (max_consecutive fires on this reference) sets
        // ref_force_reselect, which skips the retention test so the election
        // below runs even though the incumbent is perfectly healthy by
        // elevation.  Inert while the gate is disabled (the flag is only ever
        // set inside evaluateRefGate).
        bool ref_valid = false;
        if (!cs.ref_force_reselect) {
            for (const auto& sd : sat_vec)
                if (sd.satellite == cs.ref_satellite && sd.elevation >= kRefElevThreshDeg) {
                    ref_valid = true;
                    cs.ref_elevation = sd.elevation;
                    break;
                }
        }

        if (ref_valid) return false;

        // Select highest-elevation satellite
        const std::string old_ref = cs.ref_satellite;
        cs.ref_satellite.clear();
        cs.ref_elevation = -1.0;
        for (const auto& sd : sat_vec) {
            // Card R: the escalated reference is excluded from THIS election
            // only (never a permanent exclusion — rule 5: qualify, never drop).
            // ref_exclude is empty unless the gate escalated, so this branch is
            // structurally dead on the legacy path.
            if (!cs.ref_exclude.empty() && sd.satellite == cs.ref_exclude) continue;
            if (sd.elevation > cs.ref_elevation) {
                cs.ref_elevation = sd.elevation;
                cs.ref_satellite = sd.satellite;
            }
        }
        if (cs.ref_satellite.empty() && !cs.ref_exclude.empty()) {
            // The exclusion left no candidate (single-satellite family): fall
            // back to the unrestricted election rather than losing the arc.
            for (const auto& sd : sat_vec)
                if (sd.elevation > cs.ref_elevation) {
                    cs.ref_elevation = sd.elevation;
                    cs.ref_satellite = sd.satellite;
                }
        }
        cs.ref_force_reselect = false;
        cs.ref_exclude.clear();

        return !old_ref.empty() && cs.ref_satellite != old_ref;
    }

    // =================================================================
    // Card R — hardened reference-satellite slip gate
    // =================================================================

    double IfPhaseSlipDetector::updateRefDispersion(ConstellationState& cs,
        const std::vector<double>& non_ref_dds,
        double median_dd, double cold_prior, double sin_bar, double& disp_out) {

        // --- Dispersion of the family about its own median (raw m) ---------
        // The step-vs-drift discriminator.  A genuine reference slip is a
        // COMMON step: every non-reference DD moves by the same amount, so the
        // scatter about the median is the ordinary between-epoch phase noise.
        // A per-satellite drift (or a mis-modelled thin family) produces
        // scatter of the SAME order as the median it is being judged against.
        std::vector<double> devs;
        devs.reserve(non_ref_dds.size());
        for (const double d : non_ref_dds) devs.push_back(std::abs(d - median_dd));
        const double disp = 1.4826 * computeMedian(devs);
        disp_out = disp;

        // --- Effective empirical scale (empirical-Bayes, pool-arm shape) ----
        // The cold prior is the PHYSICAL floor expressed as a scale
        // (floor_frac·λ_IF / k), NOT this epoch's own dispersion.  Shrinking
        // toward the current epoch would be self-contaminating: on the very
        // first candidate the gate would reduce to "|median| > k·(this epoch's
        // scatter)", which a genuine multi-satellite station event can never
        // pass — measured, that silently swallowed the OWMG00NZL/G31 re-lock
        // the spec's R4 protects.  With this prior, zero empirical information
        // ⇒ the gate degenerates to exactly the legacy physical floor, and the
        // empirical arm takes over only as real samples accumulate.  Nothing
        // here comes from the stochastic model (project rule 4).
        const double w = static_cast<double>(cs.ref_scale_count)
                       / (static_cast<double>(cs.ref_scale_count) + ref_gate_.shrink_kappa);
        double s_eff = w * cs.ref_scale_ewma + (1.0 - w) * cold_prior;
        if (!(s_eff > 0.0)) s_eff = cold_prior;

        // Fold this epoch's dispersion in.  Called on EVERY epoch with a usable
        // family — NOT only on the epochs where the legacy condition fired —
        // so the scale is a genuine quiet-epoch noise reference rather than a
        // statistic conditioned on a large median (which would let a persistent
        // drift raise its own acceptance threshold until a real slip is missed).
        // Folding is safe on a real slip too: a COMMON step leaves the
        // dispersion at the noise level, so the scale can never be inflated by
        // the event it must catch — the opposite of folding the median would be.
        //
        // §6.14.6 item 2: under elev_norm the quantity folded is the
        // ZENITH-EQUIVALENT dispersion disp·⟨sin el⟩.  `disp` itself is the
        // family's scatter at whatever elevations happened to be up this epoch,
        // so folding it raw makes the EWMA a running mixture over a changing
        // geometry — and the state it lives in (ConstellationState) outlives
        // both the geometry and the reference satellite it is meant to describe.
        // Normalized, it is a stationary per-station receiver-noise scale, and
        // evaluateRefGate re-inflates it to the elevations that actually matter.
        const double fold = ref_gate_.elev_norm ? (disp * sin_bar) : disp;
        cs.ref_scale_ewma = (cs.ref_scale_count == 0)
            ? fold
            : (1.0 - ref_gate_.ewma_alpha) * cs.ref_scale_ewma
                  + ref_gate_.ewma_alpha * fold;
        cs.ref_scale_count += static_cast<long>(non_ref_dds.size());
        return s_eff;
    }

    IfPhaseSlipDetector::RefGateVerdict IfPhaseSlipDetector::evaluateRefGate(
        ConstellationState& cs,
        const std::vector<double>& non_ref_dds, double disp, double s_eff,
        double median_dd, double lambda_if,
        const std::string& sys_str, double sin_bar, double sin_ref) {

        auto logger = log_manager_.getLogger("if_slip_detector");
        const std::size_t n    = non_ref_dds.size();
        const double      amed = std::abs(median_dd);
        const double      phys = ref_gate_.floor_frac * lambda_if;

        // --- (0) Warm-up abstention (§6.14.6 item 1) ------------------------
        // Below shrink_kappa samples the empirical-Bayes weight w is under ½ and
        // s_eff is still mostly the cold prior floor_frac·λ_IF/k, which makes
        // k·s_eff collapse onto phys — i.e. the hardened gate silently reverts to
        // the legacy 3σ fixed bar for exactly the epochs where it has no basis to
        // decide.  Those are also the epochs where carry_warm is false, so a fire
        // can only ever be logged as `diagnostic`: it cannot refloat anything.
        // Abstaining asserts nothing about the scale (contrast the falsified
        // "shrink toward this epoch's dispersion", which asserts a great deal and
        // swallowed the OWMG00NZL/G31 re-lock); it declines to decide until the
        // empirical arm means something.  MW/LLI still screens the window.
        if (ref_gate_.warmup_abstain
            && static_cast<double>(cs.ref_scale_count) < ref_gate_.shrink_kappa) {
            ++ref_gate_counters_.suppressed_warmup;
            cs.ref_consec_fires = 0;
            logger->debug("{}: ref-gate ABSTAIN (warm-up) {} |med|={:.4f} m — "
                          "{} of {:.0f} scale sample(s); the empirical arm is not "
                          "yet informative and a cold detection cannot act",
                          sys_str, cs.ref_satellite, amed,
                          cs.ref_scale_count, ref_gate_.shrink_kappa);
            return RefGateVerdict::Suppress;
        }

        // --- (1) Thin-family honesty (R3) ----------------------------------
        // §6.14.6 item 2: under elev_norm, s_eff is a ZENITH-equivalent scale and
        // the bar is rebuilt as the sigma of the statistic actually under test —
        // the MEDIAN of the family — rather than as k× the family's raw scatter:
        //
        //   σ_ref = s_z / sin(el_ref)                 the common term.  It is in
        //           EVERY member (dd = ΔOMC(sat) − ΔOMC(ref)) so no family size
        //           averages it away — measured, σ(median) is flat 17.3–18.4 mm
        //           for n = 3…10, and 88 % of its variance is this term.
        //   σ_fam = 1.2533 · s_raw / √n               the part that DOES average
        //           down: the asymptotic SD of a median of n iid samples.
        //
        // Validated on the 24 h bed: s_z ≈ 12.8 mm at el_ref ≈ 62° gives σ_ref
        // 14.5 mm against a measured station-local common of 14.83, σ_fam
        // 10.1 mm, hypot 17.7 mm against a measured σ(median) of 17.70 mm.  The
        // flat k·s_eff form is blind to both el_ref and n; this tracks them.
        const bool   thin = (static_cast<int>(n) < ref_gate_.min_n);
        double scale = s_eff;
        if (ref_gate_.elev_norm) {
            const double s_raw   = (sin_bar > 0.0) ? (s_eff / sin_bar) : s_eff;
            const double sig_ref = (sin_ref > 0.0) ? (s_eff / sin_ref) : s_eff;
            const double sig_fam = 1.2533 * s_raw / std::sqrt(static_cast<double>(n));
            scale = std::hypot(sig_ref, sig_fam);
        }
        // The physical floor stays RAW in both modes: a slip is a slip at any
        // elevation (slip-QC lesson 7).  Only the empirical arm is normalized.
        const double thr  = thin ? std::max(ref_gate_.thin_widen * phys, ref_gate_.k * scale)
                                 : std::max(phys, ref_gate_.k * scale);
        if (!(amed > thr)) {
            if (thin) ++ref_gate_counters_.suppressed_thin;
            else      ++ref_gate_counters_.suppressed_scale;
            cs.ref_consec_fires = 0;
            logger->debug("{}: ref-gate DEFER {} |med|={:.4f} m <= thr={:.4f} "
                          "(n={}, disp={:.4f}, s_eff={:.4f}, sigma_med={:.4f}, {})",
                          sys_str, cs.ref_satellite, amed, thr, n, disp, s_eff, scale,
                          thin ? "THIN family" : "scale");
            return RefGateVerdict::Suppress;
        }

        // --- (2) Coherence: the family must AGREE on the step (R2) ----------
        // A COHERENT family (small scatter about a large median) is the only
        // signature that identifies the REFERENCE as the thing that slipped —
        // and it is the only case where rebasing prev_grad by the median is a
        // defensible claim.
        RefGateVerdict kind = RefGateVerdict::FireRebase;
        if (!(disp <= ref_gate_.coherence * amed)) {
            // Incoherent.  Two very different physical situations land here and
            // the 2026-07-21 ON smoke proved that collapsing them loses a real
            // event (the OWMG00NZL/G31 station re-lock the spec's R4 protects):
            //
            //   • DRIFT — a sub-slip trend on a thin/quiet family: the median
            //     is modest and NO individual double difference is slip-sized.
            //     Nothing happened; suppress.
            //   • STATION EVENT — a multi-satellite re-lock: many individual
            //     DDs are independently slip-sized, so the median is a MIXTURE,
            //     not a common step.  Whether the reference itself slipped is
            //     unknowable from these DDs — and under a zero-q carry the
            //     conservative action is to reset it (standing invariant #1:
            //     never let a stiff prior ride a jump), while REFUSING to claim
            //     the median as its slip magnitude.  So: fire, do not rebase.
            //
            // "Independently slip-sized" is a FULL IF cycle, not the half-cycle
            // candidate floor: a sub-slip drift can sit just above phys on every
            // member and would otherwise be promoted to a station event, which is
            // exactly the false family this card removes.
            std::size_t n_large = 0;
            for (const double d : non_ref_dds) if (std::abs(d) > lambda_if) ++n_large;
            const std::size_t need = std::max<std::size_t>(2, (n + 1) / 2);
            if (n_large >= need) {
                kind = RefGateVerdict::FireNoRebase;
                logger->info("{}: ref-gate STATION EVENT at reference {} — {} of {} "
                             "non-reference DDs are independently slip-sized "
                             "(|dd| > lambda_IF = {:.4f} m) and the family is incoherent "
                             "(disp={:.4f} m vs |med|={:.4f} m).  Resetting the "
                             "reference arc WITHOUT rebasing by the median (the "
                             "median is a mixture, not a common step).",
                             sys_str, cs.ref_satellite, n_large, n, lambda_if, disp, amed);
            } else {
                ++ref_gate_counters_.suppressed_scale;
                cs.ref_consec_fires = 0;
                logger->debug("{}: ref-gate SUPPRESS {} incoherent: disp={:.4f} m > "
                              "{:.2f}*|med|={:.4f} m (n={}, {} slip-sized) — drift, "
                              "not a common step",
                              sys_str, cs.ref_satellite, disp, ref_gate_.coherence,
                              ref_gate_.coherence * amed, n, n_large);
                return RefGateVerdict::Suppress;
            }
        }

        // --- (3) Persistence cap + escalation (R2) --------------------------
        //
        // NOT IMPLEMENTED, deliberately: spec §2's third CANDIDATE ("if the NEW
        // reference immediately slips too, the common mode is station-local —
        // hand it to the pool/RAIM layer").  That test is unreachable at this
        // seam, for two independent structural reasons, and shipping it would
        // have left a permanently-zero counter that reads as "no station-local
        // events occurred" (the standing-invariant-#4 failure mode):
        //   (a) a genuine receiver-level common mode cancels in the
        //       BETWEEN-SATELLITE single difference by construction, so it can
        //       never reach the non-reference double differences this gate
        //       adjudicates; and
        //   (b) the only epoch the test could fire on is the forced
        //       re-election epoch, where ref_changed is true and
        //       processDoubleDifferences applies the median-offset rebase —
        //       which removes exactly the common mode the test looks for
        //       before the gate is consulted.
        // Verified by construction in tests/test_if_phase_slip_detector.cpp
        // (ReferenceHandoverDoesNotManufactureASlip pins (b)).  The pool/RAIM
        // layer already owns station-local modes; the escalation below is what
        // R2 actually requires.
        const bool consecutive = (cs.ref_last_fire_epoch == epoch_counter_ - 1)
                              && (cs.ref_fire_sat == cs.ref_satellite);
        cs.ref_consec_fires    = consecutive ? (cs.ref_consec_fires + 1) : 1;
        cs.ref_fire_sat        = cs.ref_satellite;
        cs.ref_last_fire_epoch = epoch_counter_;
        if (cs.ref_consec_fires > ref_gate_.max_consecutive) {
            ++ref_gate_counters_.suppressed_persistence;
            ++ref_gate_counters_.reselects;
            cs.ref_force_reselect = true;
            cs.ref_exclude        = cs.ref_satellite;
            cs.ref_consec_fires   = 0;
            logger->warn("{}: ref-gate ESCALATE — {} fired {} consecutive epoch(s) "
                         "(|med|={:.4f} m, disp={:.4f} m): a persistent drift, not a "
                         "step.  Forcing reference re-selection; the satellite moves to "
                         "the pool arm.", sys_str, cs.ref_fire_sat,
                         ref_gate_.max_consecutive + 1, amed, disp);
            return RefGateVerdict::Suppress;
        }

        ++ref_gate_counters_.fires;
        if (kind == RefGateVerdict::FireNoRebase) ++ref_gate_counters_.fires_station_event;
        return kind;
    }

    // =================================================================
    // Double-difference processing
    // =================================================================

    std::map<std::string, double> IfPhaseSlipDetector::processDoubleDifferences(
        const std::vector<SatData>& sat_vec,
        const std::string& sys_str,
        ConstellationState& cs,
        const std::map<std::string, double>& grad_phi_if,
        double lambda_if,
        bool ref_changed,
        double position_sigma_3d,
        const Eigen::Vector3d& delta_pos,
        std::vector<std::string>& reset_keys,
        bool run_ref_arm,
        RefArmOutcome* out_outcome,
        const RefArmOutcome* inherit) {

        auto logger = log_manager_.getLogger("if_slip_detector");

        // §22.6 SECOND LOCK.  An ADDITIONAL chain may run the arm only through
        // the EMPIRICAL gate.  With ref_gate_.enable off the legacy candidate
        // condition |median| > floor_frac·λ_IF IS the decision, and that is the
        // §14.10.3 flood: on (L2W,L5Q) the IF coefficients are 12.26 / −11.26
        // (noise amplification 16.6) against λ_IF = 0.1248 m, so NOISE clears it
        // — 5 351 candidates / 64 fires on a quiet 70-epoch null.  The loader
        // disarms if_phase_td_ref_arm_all without the gate; this refuses the
        // combination in code, where the invariant actually lives.
        const bool extra_chain = (sys_str.find('|') != std::string::npos);
        if (extra_chain && !ref_gate_.enable) {
            run_ref_arm = false;
            inherit     = nullptr;
        }
        // ONE definition, set by detect() — position uncertainty OR model nuisance.
        // Re-deriving it from position_sigma_3d here would silently exclude the
        // §6.14.6 item 3 arm and leave the reference gate firing through a closed gate.
        const bool pos_gated = epoch_gated_;

        // Build sat lookup
        std::map<std::string, const SatData*> sat_map;
        for (const auto& sd : sat_vec) sat_map[sd.satellite] = &sd;

        // Find reference SatData
        const SatData* ref_data = sat_map.count(cs.ref_satellite) ? sat_map.at(cs.ref_satellite) : nullptr;
        if (!ref_data) return {};

        // --- Handle reference change: apply median offset to prev gradients ---
        if (ref_changed && !cs.prev_grad.empty()) {
            std::vector<double> offsets;
            for (const auto& [sat, g] : grad_phi_if) {
                auto prev_it = cs.prev_grad.find(sat);
                if (prev_it != cs.prev_grad.end())
                    offsets.push_back(g - prev_it->second);
            }
            if (!offsets.empty()) {
                const double offset = computeMedian(offsets);
                for (auto& [sat, prev_g] : cs.prev_grad) prev_g += offset;
                logger->debug("{}: Reference changed to {} (elev {:.1f}), applied median offset {:.6f} m",
                    sys_str, cs.ref_satellite, cs.ref_elevation, offset);
            }
        }

        // --- Compute between-epoch double differences ---
        std::map<std::string, double> delta_grad;
        bool all_finite = true;
        for (const auto& [sat, g] : grad_phi_if) {
            auto prev_it = cs.prev_grad.find(sat);
            if (prev_it != cs.prev_grad.end()) {
                delta_grad[sat] = g - prev_it->second;
            } else {
                delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                all_finite = false;
            }
        }
        for (const auto& [sat, dd] : delta_grad)
            if (!std::isfinite(dd)) { all_finite = false; break; }

        // --- Geometry correction: remove (e_sat - e_ref) · ?pos ---
        // The between-satellite single-difference does NOT perfectly cancel
        // geometry.  The epoch-to-epoch position change produces a residual
        // of (e_sat - e_ref) · ?pos in each double difference.  Correct it
        // using the current-epoch LOS vectors and the position change.
        if (delta_pos.norm() > 1e-6) {
            const Eigen::Vector3d& ref_los = ref_data->los;
            for (auto& [sat, dd] : delta_grad) {
                if (!std::isfinite(dd)) continue;
                auto sit = sat_map.find(sat);
                if (sit == sat_map.end()) continue;
                const double geom_correction = (sit->second->los - ref_los).dot(delta_pos);
                dd -= geom_correction;
            }
        }

        if (pos_gated) {
            // Return geometry-corrected DDs for tracing/caching but suppress slip flagging
            return delta_grad;
        }

        // --- Reference satellite slip check (runs regardless of all_finite) ---
        // §14.10.4: ONE reference arm per (station, constellation).  The
        // ADDITIONAL chains of `if_phase_td_pairs = all` share the first chain's
        // reference satellite and do NOT re-run its test — running it three times
        // is not a pure addition, it is three tests on three DIFFERENT iono-free
        // combinations of the same arcs, and the extra-wide ones are
        // ill-conditioned: (L2W,L5Q) has IF coefficients 12.26 / −11.26 (noise
        // amplification 16.6 against 2.98 on (L1C,L2W)) while its λ_IF is
        // 0.1248 m, so the legacy candidate condition |median(non-ref ΔΔ)| >
        // 0.5·λ_IF is met by NOISE.  Measured on a 70-epoch quiet 60-station null:
        // legacy candidates 25 → 5 351 and FIRES 1 → 64 (52 of them marked
        // incoherent multi-satellite STATION EVENTS), with the POOL arm flagging
        // ZERO extra — a false-alarm cost entirely invisible in the per-pair
        // evaluated/flagged tally.  Sharing the reference keeps every chain's
        // gradients in one frame and leaves the screening where it was.
        // §22.6: the ACTION of a reference-arm fire, shared by the chain's own
        // arm and by the ADOPTED-reference inheritance path below, so the two
        // cannot drift apart.  `inherited` only changes the telemetry — the
        // reset key, the rebase and the trace record are the same response,
        // because the claim being acted on ("this chain's reference stepped") is
        // the same claim whichever arm established it.
        auto applyRefFire = [&](double median_dd, RefGateVerdict verdict,
                                std::size_t n_finite, bool inherited) {
            auto& rc = ref_arm_chain_counters_[sys_str];
            if (inherited) ++rc.fires_inherited; else ++rc.fires_own;

            const std::string ref_amb_key = cs.ref_satellite + "_" + ref_data->phase_if_key;
            reset_keys.push_back(ref_amb_key);

            // The OFF-path text is preserved verbatim; only an INHERITED fire
            // adds a clause, so an existing log line cannot change under the knob.
            if (inherited)
                logger->warn("{}: Reference satellite {} IF phase slip INHERITED from the "
                    "first chain's verdict: median(delta_grad)={:.4f} m ({:.1f} IF cyc), "
                    "lambda_IF={:.4f} m (n_finite={}); reset {}",
                    sys_str, cs.ref_satellite, median_dd,
                    median_dd / lambda_if, lambda_if, n_finite, ref_amb_key);
            else
                logger->warn("{}: Reference satellite {} IF phase slip: "
                    "median(delta_grad)={:.4f} m ({:.1f} IF cyc) > 0.5*lambda_IF={:.4f} m "
                    "(n_finite={}); reset {}",
                    sys_str, cs.ref_satellite, median_dd,
                    median_dd / lambda_if, 0.5 * lambda_if,
                    n_finite, ref_amb_key);

            writeTraceRecord(*ref_data, sys_str, 0.0, median_dd,
                cs.ref_satellite, false, lambda_if, true, "ref_slip",
                position_sigma_3d, false);

            // Correct prev gradients by the median offset and recompute double
            // diffs so that subsequent outlier detection - here, or the network
            // pool the driver runs on the cached DDs - sees only the
            // per-satellite residual after removing the reference slip.  THIS is
            // what stops the fan-out: without it the reference's own step rides
            // every non-reference DD of the chain and is attributed to innocent
            // arcs (§22.6: six false refloats on G11/G25/G32).
            //
            // Card R: SKIPPED for a FireNoRebase verdict.  The rebase asserts
            // "the reference slipped by exactly median_dd", which is only
            // defensible for a coherent common step.  On an incoherent
            // multi-satellite station event the median is a mixture, and
            // subtracting it from every OTHER satellite's history would inject a
            // fictitious step into arcs that are fine - while also hiding the
            // real per-satellite jumps from the network pool arm downstream.
            // Resetting the reference arc (above) is kept; inventing its
            // magnitude is not.
            if (verdict != RefGateVerdict::FireRebase) return;

            // Fan-out rows this rebase de-implicates: non-reference DDs above the
            // chain's own physical floor before it and below after.  The honest
            // ON-path number for "what would otherwise have been attributed to an
            // innocent arc" (invariant #4) - a floor-crossing count, not a claim
            // about what the network pool would have flagged.
            const double floor_m = ref_gate_.floor_frac * lambda_if;
            for (const auto& [sat, dd] : delta_grad)
                if (sat != cs.ref_satellite && std::isfinite(dd)
                    && std::abs(dd) > floor_m && std::abs(dd - median_dd) <= floor_m)
                    ++rc.fanout_suppressed;

            for (auto& [sat, prev_g] : cs.prev_grad)
                if (sat != cs.ref_satellite) prev_g += median_dd;

            delta_grad.clear();
            all_finite = true;
            for (const auto& [sat, g] : grad_phi_if) {
                auto prev_it = cs.prev_grad.find(sat);
                if (prev_it != cs.prev_grad.end()) {
                    delta_grad[sat] = g - prev_it->second;
                } else {
                    delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                    all_finite = false;
                }
            }
        };

        if (run_ref_arm)
        // Collect finite non-reference double differences. If the median is
        // large, the reference satellite slipped  not the non-ref satellites.
        // This must run before individual outlier detection so that the
        // common-mode reference slip is removed first.
        {
            std::vector<double> non_ref_dds;
            // §6.14.6 item 2 geometry: the mean sin(elevation) of the family that
            // forms the median, and sin(el) of the reference whose own noise is
            // the median's dominant term.  Both clamped so a horizon arc cannot
            // explode 1/sin(el).  Computed unconditionally (a few flops) but READ
            // only under ref_gate_.elev_norm, so the OFF path stays byte-identical.
            constexpr double kDegToRad = 0.017453292519943295;
            double sin_sum = 0.0;
            int    sin_n   = 0;
            const double sin_clamp =
                std::sin(std::max(ref_gate_.elev_clamp_deg, 0.0) * kDegToRad);
            for (const auto& [sat, dd] : delta_grad)
                if (sat != cs.ref_satellite && std::isfinite(dd)) {
                    non_ref_dds.push_back(dd);
                    auto sit = sat_map.find(sat);
                    if (sit != sat_map.end()) {
                        sin_sum += std::max(std::sin(sit->second->elevation * kDegToRad),
                                            sin_clamp);
                        ++sin_n;
                    }
                }
            const double sin_bar = (sin_n > 0) ? (sin_sum / sin_n) : 1.0;
            const double sin_ref =
                std::max(std::sin(ref_data->elevation * kDegToRad), sin_clamp);

            if (non_ref_dds.size() >= 2) {
                // Tier T1r opportunity (invariant #4): a usable family existed, so
                // this station-epoch-constellation was screenable by the reference
                // arm.  Counted UNCONDITIONALLY — the legacy condition below runs
                // whether or not the hardened gate is armed, and its own denominator
                // is NOT the pool arm's per-(station,sat,pair) sample count, which
                // is exactly why T1r must stay a separate tier from T1.
                ++ref_gate_counters_.gate_opportunities;
                // 22.6: the SAME tier, split per gradient chain.  The aggregate
                // above sums every chain into one number and so cannot answer
                // whether the ADDITIONAL chains' arm is a flood or a backstop.
                auto& rc_own = ref_arm_chain_counters_[sys_str];
                ++rc_own.opportunities;
                const double median_dd = computeMedian(non_ref_dds);

                // Card R: the LEGACY condition is the candidate filter; when the
                // hardened gate is armed it then adjudicates (empirical scale +
                // coherence + thin-family + persistence).  Disabled ⇒ the legacy
                // condition IS the decision ⇒ byte-identical.
                RefGateVerdict verdict =
                    (std::abs(median_dd) > ref_gate_.floor_frac * lambda_if)
                        ? RefGateVerdict::FireRebase
                        : RefGateVerdict::Suppress;
                if (verdict != RefGateVerdict::Suppress) ++rc_own.candidates;
                if (ref_gate_.enable) {
                    // The dispersion scale advances on EVERY epoch with a usable
                    // family, candidate or not (see updateRefDispersion).
                    double disp = 0.0;
                    const double s_eff = updateRefDispersion(
                        cs, non_ref_dds, median_dd,
                        (ref_gate_.k > 0.0)
                            ? (ref_gate_.floor_frac * lambda_if / ref_gate_.k)
                            : (ref_gate_.floor_frac * lambda_if),
                        sin_bar, disp);
                    if (verdict != RefGateVerdict::Suppress) {
                        ++ref_gate_counters_.legacy_candidates;
                        verdict = evaluateRefGate(cs, non_ref_dds, disp, s_eff,
                                                  median_dd, lambda_if, sys_str,
                                                  sin_bar, sin_ref);
                    } else {
                        // A quiet epoch breaks any consecutive-fire run.
                        cs.ref_consec_fires = 0;
                    }
                }
                const bool ref_fire = (verdict != RefGateVerdict::Suppress);
                if (ref_fire) ++ref_gate_counters_.gate_flags;  // tier T1r numerator

                if (ref_fire) applyRefFire(median_dd, verdict, non_ref_dds.size(), false);

                // 22.6: publish this chain's verdict so the ADDITIONAL chains that
                // ADOPTED its reference can inherit it.  Recorded whether or not it
                // fired: the inheritance path must be able to tell "the arm ran and
                // found nothing" (exactly today) from "the arm did not run".
                if (out_outcome) {
                    out_outcome->satellite = cs.ref_satellite;
                    out_outcome->verdict   = verdict;
                    out_outcome->fired     = ref_fire;
                }
            }
        }

        // --- 22.6: the ADOPTED-reference inheritance path -------------------
        // An additional chain that adopted the first chain's reference has no
        // arm of its own by construction (its reference's own DD is identically
        // 0), and today the first chain's fire does not reach it: the step stays
        // on every one of THIS chain's non-reference DDs and is attributed to
        // innocent arcs by the pooled test.  That fan-out is the defect (six
        // false refloats on GOPE00CZE G11/G25/G32 from E1's slip on G24).
        //
        // The verdict is inherited, never re-derived: re-testing would be the
        // 14.10.3 flood on a wide combination.  The MAGNITUDE is this chain's
        // own median of its own non-reference DDs, because the step in metres is
        // per-combination (the same integer slip moves each IF pair by its own
        // coefficients) - only the QUESTION "did this satellite step?" is shared.
        if (!run_ref_arm && inherit && inherit->fired
            && !inherit->satellite.empty()
            && inherit->satellite == cs.ref_satellite) {
            std::vector<double> non_ref_dds;
            for (const auto& [sat, dd] : delta_grad)
                if (sat != cs.ref_satellite && std::isfinite(dd))
                    non_ref_dds.push_back(dd);
            if (non_ref_dds.size() >= 2) {
                auto& rc = ref_arm_chain_counters_[sys_str];
                ++rc.opportunities;
                const double median_dd = computeMedian(non_ref_dds);
                if (std::abs(median_dd) > ref_gate_.floor_frac * lambda_if)
                    ++rc.candidates;
                applyRefFire(median_dd, inherit->verdict, non_ref_dds.size(), true);
            }
        }

        // --- Non-reference outlier detection ---
        // Deferred mode (network producer): the per-satellite outlier flag is done
        // by the driver against a NETWORK-POOLED per-signal-pair MAD; skip it here.
        // The between-epoch double differences are still computed, cached, and
        // carried forward. The reference-slip common-mode check above still runs.
        if (!defer_flagging_) {
        if (all_finite && delta_grad.size() >= 2) {
            // Full MAD-based adaptive outlier check
            auto cleaned = checkOutliers(delta_grad, cs.ref_satellite, lambda_if);
            for (const auto& [sat, dd] : cleaned) {
                if (sat == cs.ref_satellite) continue;
                if (!std::isfinite(dd) && std::isfinite(delta_grad[sat])) {
                    auto sit = sat_map.find(sat);
                    if (sit == sat_map.end()) continue;
                    const std::string amb_key = sat + "_" + sit->second->phase_if_key;
                    reset_keys.push_back(amb_key);

                    logger->warn("{}: IF phase slip for {}: delta_grad={:.4f} m, lambda_IF={:.4f} m; reset {}",
                        sys_str, sat, delta_grad[sat], lambda_if, amb_key);

                    writeTraceRecord(*sit->second, sys_str,
                        grad_phi_if.at(sat), delta_grad.at(sat),
                        cs.ref_satellite, ref_changed, lambda_if,
                        true, "non_ref_outlier", position_sigma_3d, false);
                }
            }
        } else {
            // Incomplete data: apply fixed threshold on available diffs
            for (const auto& [sat, dd] : delta_grad) {
                if (!std::isfinite(dd) || std::abs(dd) <= lambda_if) continue;
                auto sit = sat_map.find(sat);
                if (sit == sat_map.end()) continue;
                const std::string amb_key = sat + "_" + sit->second->phase_if_key;
                reset_keys.push_back(amb_key);

                logger->warn("{}: IF phase slip (fixed threshold) for {}: |delta_grad|={:.4f} m > lambda_IF={:.4f} m; reset {}",
                    sys_str, sat, std::abs(dd), lambda_if, amb_key);

                writeTraceRecord(*sit->second, sys_str,
                    grad_phi_if.count(sat) ? grad_phi_if.at(sat)
                        : std::numeric_limits<double>::quiet_NaN(),
                    dd, cs.ref_satellite, ref_changed, lambda_if,
                    true, "fixed_threshold", position_sigma_3d, false);
            }
        }
        } // if (!defer_flagging_)

        return delta_grad;
    }

    // =================================================================
    // Top-level detect()
    // =================================================================

    std::vector<std::string> IfPhaseSlipDetector::detect(
        const std::vector<PppObservation>& observations,
        double position_sigma_3d,
        const Eigen::Vector3d& receiver_position,
        double model_nuisance_m) {

        std::vector<std::string> reset_keys;
        auto logger = log_manager_.getLogger("if_slip_detector");
        last_if_jumps_.clear();
        last_if_jumps_all_.clear();
        // Card R: monotonic detect() index — the unit the reference-gate
        // persistence run-length is measured in.  Incremented unconditionally
        // (inert while the gate is disabled) so the counter never depends on
        // which constellations happened to be processed.
        ++epoch_counter_;

        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        // §6.14.6 item 3: the model-nuisance gate is the SIBLING of the position
        // gate, for the term that actually survives on the network path.  Both
        // land in the same `pos_gated` flag because the response is identical —
        // keep differencing and keep the gradient chain warm, but do not flag on
        // a statistic whose null is invalid this epoch.
        const bool nuisance_gated =
            (nuisance_max_m_ > 0.0) && (model_nuisance_m > nuisance_max_m_);
        const bool pos_gated = (position_sigma_3d > kPositionSigmaThresholdM) || nuisance_gated;
        epoch_gated_ = pos_gated;   // the single definition processDoubleDifferences reads
        if (pos_gated)
            logger->debug("IF slip detection gated: position_sigma_3d={:.3f} m (thr {:.2f}), "
                "model_nuisance={:.4f} m (thr {:.4f}, 0 = off)",
                position_sigma_3d, kPositionSigmaThresholdM,
                model_nuisance_m, nuisance_max_m_);

        auto by_constellation = groupByConstellation(observations);

        // §14.10: one iteration per GRADIENT CHAIN, not per constellation.  With
        // the pair width at `first` there is exactly one chain per system and
        // `sys_str` is the bare system string, so everything below — including
        // every log line, which names the chain — is the pre-knob text.  With it
        // at `all` the additional chains are named "GPS|L1C_L5Q" and are wholly
        // independent: own reference election, own prev_grad, own reset keys.
        // §14.10.4: the FIRST chain's elected reference per system.  std::map
        // orders "GPS" before "GPS|…" ('|' > every alphanumeric), so the first
        // chain of a system is always processed before its additional chains and
        // this is filled by the time they need it.
        std::map<std::string, std::string> first_ref_by_sys;
        // §22.6: and its reference arm's VERDICT this epoch, for the additional
        // chains that adopted it.  Same ordering argument — the first chain has
        // already run by the time an additional chain of its system is reached.
        std::map<std::string, RefArmOutcome> first_ref_arm_by_sys;

        for (auto& [sys_str, sat_vec] : by_constellation) {
            if (sat_vec.size() < 2) continue;

            // The first pair's chain is keyed by the bare system string.
            const std::size_t bar = sys_str.find('|');
            const bool first_pair_chain = (bar == std::string::npos);
            const std::string sys_only = first_pair_chain ? sys_str : sys_str.substr(0, bar);

            auto& cs = constellation_state_[sys_str];

            // Compute epoch-to-epoch position change for geometry correction.
            Eigen::Vector3d delta_pos = Eigen::Vector3d::Zero();
            if (cs.has_prev_position && receiver_position.norm() > 1e-6) {
                delta_pos = receiver_position - cs.prev_position;
            }

            // Median IF wavelength
            std::vector<double> lambdas;
            lambdas.reserve(sat_vec.size());
            for (const auto& sd : sat_vec) lambdas.push_back(sd.lambda_if);
            const double lambda_if = computeMedian(lambdas);

            // Reference selection.  §14.10.4: an ADDITIONAL chain does not hold
            // its own election — it adopts the first chain's reference whenever
            // that satellite is present here, so the whole satellite's pairs sit
            // in ONE between-satellite frame and the reference arm (which only
            // the first chain runs) still covers the reference it screens.  The
            // fallback to its own election matters only for a satellite set that
            // does not contain the first chain's reference at all, e.g. a
            // satellite tracking L1/L2 but not L5.
            bool ref_changed;
            bool adopted_ref = false;        // §22.6: which of the two paths ran
            if (first_pair_chain) {
                ref_changed = selectReference(sat_vec, cs);
                first_ref_by_sys[sys_only] = cs.ref_satellite;
            } else {
                const auto fit = first_ref_by_sys.find(sys_only);
                bool adopted = false;
                if (fit != first_ref_by_sys.end() && !fit->second.empty())
                    for (const auto& sd : sat_vec)
                        if (sd.satellite == fit->second) {
                            ref_changed = (cs.ref_satellite != fit->second);
                            cs.ref_satellite = fit->second;
                            cs.ref_elevation = sd.elevation;
                            adopted = true;
                            break;
                        }
                if (!adopted) ref_changed = selectReference(sat_vec, cs);
                adopted_ref = adopted;
            }

            // §22.6: the two backstops for a slip ON an additional chain's
            // reference, both OFF unless ref_arm_all_ is armed.
            //   OWN-ELECTED  ⇒ run the arm on this chain, through the EMPIRICAL
            //                  gate, on this chain's own cs.ref_scale_* state and
            //                  this chain's own λ_IF.  Requires ref_gate_.enable:
            //                  the legacy 0.5·λ_IF candidate condition IS the
            //                  §14.10.3 flood on a wide combination, and
            //                  processDoubleDifferences refuses the pair again.
            //   ADOPTED      ⇒ inherit the first chain's verdict for that
            //                  satellite this epoch (no second test, so no flood).
            const bool run_extra_own_arm =
                !first_pair_chain && ref_arm_all_ && ref_gate_.enable && !adopted_ref;
            const RefArmOutcome* inherit = nullptr;
            if (!first_pair_chain && ref_arm_all_ && adopted_ref) {
                const auto oit = first_ref_arm_by_sys.find(sys_only);
                if (oit != first_ref_arm_by_sys.end()) inherit = &oit->second;
            }

            // Find reference data
            const SatData* ref_data = nullptr;
            for (const auto& sd : sat_vec)
                if (sd.satellite == cs.ref_satellite) { ref_data = &sd; break; }
            if (!ref_data) continue;

            // Between-satellite single differencing
            std::map<std::string, double> grad_phi_if;
            for (const auto& sd : sat_vec)
                grad_phi_if[sd.satellite] = sd.phi_if_omc - ref_data->phi_if_omc;

            // Double-difference processing (if previous epoch exists).
            // Returns geometry-corrected, reference-slip-corrected delta_grads.
            std::map<std::string, double> delta_grad;
            if (!cs.prev_grad.empty()) {
                RefArmOutcome outcome;
                delta_grad = processDoubleDifferences(sat_vec, sys_str, cs, grad_phi_if,
                    lambda_if, ref_changed, position_sigma_3d, delta_pos, reset_keys,
                    /*run_ref_arm=*/first_pair_chain || run_extra_own_arm,  // §14.10.4 / §22.6
                    /*out_outcome=*/first_pair_chain ? &outcome : nullptr,
                    /*inherit=*/inherit);
                if (first_pair_chain && !outcome.satellite.empty())
                    first_ref_arm_by_sys[sys_only] = outcome;
            }

            // For satellites without a previous epoch, fill NaN
            for (const auto& [sat, g] : grad_phi_if)
                if (delta_grad.find(sat) == delta_grad.end())
                    delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();

            // Build per-satellite lookup for caching: elevation, the IF pair
            // signature (sig1/sig2), and the per-pair IF wavelength c/(f1+f2) =
            // the (1,1) slip jump, used as the physical detection floor downstream.
            std::map<std::string, const SatData*> sat_lookup;
            for (const auto& sd : sat_vec)
                sat_lookup[sd.satellite] = &sd;

            // Write trace records for non-slip satellites
            if (trace_file_.is_open()) {
                for (const auto& sd : sat_vec) {
                    bool was_reset = false;
                    for (const auto& key : reset_keys)
                        if (key.find(sd.satellite) == 0) { was_reset = true; break; }
                    if (was_reset) continue;

                    writeTraceRecord(sd, sys_str,
                        grad_phi_if.count(sd.satellite) ? grad_phi_if.at(sd.satellite)
                            : std::numeric_limits<double>::quiet_NaN(),
                        delta_grad.count(sd.satellite) ? delta_grad.at(sd.satellite)
                            : std::numeric_limits<double>::quiet_NaN(),
                        cs.ref_satellite, ref_changed, lambda_if,
                        false, pos_gated ? "pos_gated" : "ok",
                        position_sigma_3d, pos_gated);
                }
            }

            // Cache IF jump values for external query (network pool / joint MW+IF).
            for (const auto& [sat, dd] : delta_grad) {
                IfJumpInfo info;
                info.delta_grad_m = dd;
                auto sdit = sat_lookup.find(sat);
                if (sdit != sat_lookup.end()) {
                    info.lambda_if_m   = sdit->second->lambda_if;   // per-pair (1,1) floor
                    info.elevation_deg = sdit->second->elevation;
                    info.sig1          = sdit->second->sig1;
                    info.sig2          = sdit->second->sig2;
                } else {
                    info.lambda_if_m = lambda_if;                   // fallback: constellation median
                }
                // §6.14.6 item 3, the GATING LEAK.  A gated epoch suppressed this
                // detector's own flagging but still published its jumps as VALID,
                // and the network pool arm admits any sample with `jp.valid` — so
                // the pool kept thresholding double differences the detector had
                // just declared untrustworthy.  The gate has to mean the same thing
                // to both arms or it protects neither.  The jumps are still cached
                // (a downstream consumer may want the number) and the gradient
                // chain still advances, so detection resumes seamlessly.
                info.valid        = std::isfinite(dd) && !pos_gated;
                info.is_reference = (sat == cs.ref_satellite);
                // §14.10: the chain this sample rode, so a driver that flags it
                // can restart THAT chain and only that one.
                info.state_key    = sys_str;
                // The all-pairs cache is a superset and is always filled; the
                // per-satellite one keeps its pre-knob contract (the FIRST pair).
                if (!info.sig1.empty() && !info.sig2.empty())
                    last_if_jumps_all_[{ sat, info.sig1 + "_" + info.sig2 }] = info;
                if (first_pair_chain) last_if_jumps_[sat] = info;
            }

            // Store gradients for next epoch (exclude reset satellites)
            cs.prev_grad.clear();
            cs.prev_los.clear();
            for (const auto& [sat, g] : grad_phi_if) {
                bool was_reset = false;
                for (const auto& key : reset_keys)
                    if (key.find(sat + "_") == 0) { was_reset = true; break; }
                if (!was_reset) {
                    cs.prev_grad[sat] = g;
                    auto sit_it = std::find_if(sat_vec.begin(), sat_vec.end(),
                        [&](const SatData& sd) { return sd.satellite == sat; });
                    if (sit_it != sat_vec.end())
                        cs.prev_los[sat] = sit_it->los;
                }
            }

            // Store receiver position for geometry correction on next epoch.
            if (receiver_position.norm() > 1e-6) {
                cs.prev_position = receiver_position;
                cs.has_prev_position = true;
            }
        }

        return reset_keys;
    }

    // =================================================================
    // External reset
    // =================================================================

    void IfPhaseSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("if_slip_detector");
        for (auto& [sys, cs] : constellation_state_) {
            auto it = cs.prev_grad.find(satellite);
            if (it != cs.prev_grad.end()) {
                logger->debug("Cleared IF phase history for {} in constellation {}", satellite, sys);
                cs.prev_grad.erase(it);
                cs.prev_los.erase(satellite);
            }
            if (cs.ref_satellite == satellite) {
                logger->debug("Reference satellite {} for {} was reset, will reselect next epoch", satellite, sys);
                cs.ref_satellite.clear();
                cs.ref_elevation = 0.0;
            }
        }
    }

    void IfPhaseSlipDetector::resetSatelliteInState(const std::string& state_key,
        const std::string& satellite) {
        // §14.10.  Scoped to ONE gradient chain: a flag on the (L1C,L5Q) pair must
        // not throw away the (L1C,L2W) chain, which did not flag.  Same two
        // actions resetSatellite takes, on one entry instead of every entry.
        auto sit = constellation_state_.find(state_key);
        if (sit == constellation_state_.end()) return;
        auto& cs = sit->second;
        auto logger = log_manager_.getLogger("if_slip_detector");
        auto it = cs.prev_grad.find(satellite);
        if (it != cs.prev_grad.end()) {
            logger->debug("Cleared IF phase history for {} in chain {}", satellite, state_key);
            cs.prev_grad.erase(it);
            cs.prev_los.erase(satellite);
        }
        if (cs.ref_satellite == satellite) {
            logger->debug("Reference satellite {} for chain {} was reset, will reselect next epoch",
                satellite, state_key);
            cs.ref_satellite.clear();
            cs.ref_elevation = 0.0;
        }
    }

    void IfPhaseSlipDetector::resetAll() {
        constellation_state_.clear();
        last_if_jumps_.clear();
        last_if_jumps_all_.clear();
        log_manager_.getLogger("if_slip_detector")->debug("All IF phase slip state cleared");
    }

    IfPhaseSlipDetector::IfJumpInfo IfPhaseSlipDetector::getLastIfJump(
        const std::string& satellite) const {
        auto it = last_if_jumps_.find(satellite);
        if (it != last_if_jumps_.end())
            return it->second;
        return {};
    }

} // namespace GnssProcessor

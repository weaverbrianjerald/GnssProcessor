/*!
 * \file IfPhaseSlipDetector.cpp
 * \brief Between-satellite, between-epoch IF phase slip detector.
 *
 * Organization:
 *  - Construction / the slip log line
 *  - Signal helpers
 *  - Statistics (median)
 *  - Observation grouping / reference selection
 *  - The hardened reference-satellite gate
 *  - Double-difference processing: processDoubleDifferences, a short driver
 *    over the steps D1 to D7 that share one ChainWork
 *    (slip/IfPhaseChainWork.hpp)
 *  - Top-level detect()
 *  - External reset and re-seed
 * The branches of the staged user path that no shipped configuration reaches
 * (the combined IF-label input, the position and model-nuisance gates, the
 * geometry correction, the per-station outlier flagging) are in
 * IfPhaseStagedUserPath.cpp.
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/IfPhaseChainWork.hpp"
#include "common/Constants.hpp"
#include "math/RobustStats.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace GnssProcessor {

    // =================================================================
    // Construction / the slip log line
    // =================================================================

    IfPhaseSlipDetector::IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("if_slip_detector")->info(
            "IfPhaseSlipDetector initialized: ref_elev_thresh={:.0f} deg, outlier_sigma={:.1f}, pos_sigma_gate={:.2f} m",
            kRefElevThreshDeg, kOutlierSigmaMultiplier, kPositionSigmaThresholdM);
    }

    void IfPhaseSlipDetector::logSlipRecord(const SatData& sd,
        const std::string& sys_str,
        double grad, double delta_grad,
        const std::string& ref_sat,
        double lambda_if,
        const std::string& slip_reason,
        double position_sigma_3d) const {

        auto logger = log_manager_.getLogger("if_slip_detector");

        logger->info("[W{} {:.3f}] {} {} elev={:.1f} grad={:.6f} delta_grad={:.6f} "
            "ref={} lambda_if={:.6f} slip_reason={} pos_sigma={:.3f}",
            current_epoch_.week, current_epoch_.tow,
            sd.satellite, sys_str, sd.elevation,
            grad, delta_grad, ref_sat, lambda_if, slip_reason,
            position_sigma_3d);
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
    // Statistics (median; the MAD outlier check is staged)
    // =================================================================

    double IfPhaseSlipDetector::computeMedian(std::vector<double>& values) {
        // Delegates to the shared RobustStats::median, which does not sort
        // `values` in place.  Every call site uses only the return value.
        return RobustStats::median(values);
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
                groupCombinedIfRow(obs, phase_if_key, result);  // STAGED (IfPhaseStagedUserPath.cpp)
            } else {
                groupUncombinedRows(obs, result);
            }
        }

        return result;
    }

    void IfPhaseSlipDetector::groupUncombinedRows(const PppObservation& obs,
        std::map<std::string, std::vector<SatData>>& result) const {
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
        if (phase_signals.size() < 2) return;

        // Pair width (§14.10).  With all_pairs_ off, the first two phase
        // signals in key order form the one IF combination, filed under the
        // bare system string.  With it on, every distinct pair is formed; the
        // first pair keeps that chain (so its population, reference election,
        // double differences and log lines are unchanged) and each additional
        // pair gets a chain of its own, "<system>|<sig1>_<sig2>", with its own
        // reference, gradient history and reset key.
        //
        // With `network.second_phase_channel`, two of these signals can share a
        // frequency (L1C beside L1W); index pair (0, 1) then fails the `denom`
        // guard below and files nothing.  Assigning the bare-system chain by
        // index would leave it empty: every valid pair would land on a
        // "<system>|<sig1>_<sig2>" chain, and the chain that owns the reference
        // election, the gradient history and the reset keys would receive no
        // rows.  The bare-system chain therefore goes to the first pair with
        // distinct frequencies, tracked by if_pair_taken.  When no two phases
        // share a frequency that pair is (0, 1) and the output is
        // byte-identical, also when (0, 1) passes the frequency guard and then
        // fails a later check, because the flag is set at the frequency guard,
        // where an index test would also have committed (card SID phase 6a).
        const std::string sys_key = PppUtils::getSystemString(obs.system);
        const std::size_t n_use =
            all_pairs_ ? phase_signals.size() : std::size_t{ 2 };
        bool if_pair_taken = false;
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
        // Claim the bare-system chain here, at the frequency guard, and not at
        // the row push (see the comment above the loop; card SID phase 6a).
        const bool is_first_pair = !if_pair_taken;
        if_pair_taken = true;

        const double c1 = f1sq / denom;
        const double c2 = -f2sq / denom;

        const double omc1 = obs1 - mod1;
        const double omc2 = obs2 - mod2;
        const double phi_if_omc = c1 * omc1 + c2 * omc2;
        if (!std::isfinite(phi_if_omc)) continue;

        const double lambda_if = computeIfWavelengthMeters(obs, sig1, sig2);
        if (lambda_if <= 0.0) continue;

        const std::string synth_key = sig1 + "_" + sig2;

        result[is_first_pair ? sys_key : (sys_key + "|" + synth_key)].push_back(
            { obs.satellite, synth_key, sig1, sig2,
              phi_if_omc, obs.elevation, lambda_if, obs.system, obs.los });
        }  // b: second band of the pair
        }  // a: first band of the pair
    }

    bool IfPhaseSlipDetector::selectReference(const std::vector<SatData>& sat_vec,
        ConstellationState& cs) const {

        // Keep the current reference if it is present at or above
        // kRefElevThreshDeg.  An escalation of the reference gate (more than
        // max_consecutive fires on this reference) sets ref_force_reselect, which
        // skips this retention test so the election below runs whatever the
        // incumbent's elevation.  Only evaluateRefGate sets the flag, so this is
        // inert while the gate is disabled.
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
            // The escalated reference is excluded from this election only, never
            // permanently (rule 5: qualify, never drop).  ref_exclude is empty
            // unless the gate escalated, so the legacy path never takes this
            // branch.
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
    // Hardened reference-satellite slip gate
    // =================================================================

    double IfPhaseSlipDetector::updateRefDispersion(ConstellationState& cs,
        const std::vector<double>& non_ref_dds,
        double median_dd, double cold_prior, double sin_bar, double& disp_out) {

        // --- Dispersion of the family about its own median (raw m) ---------
        // Separates a step from a drift.  A genuine reference slip is a common
        // step: every non-reference DD moves by the same amount, so the scatter
        // about the median is the ordinary between-epoch phase noise.  A
        // per-satellite drift (or a mis-modelled thin family) produces scatter
        // of the same order as the median it is judged against.
        std::vector<double> devs;
        devs.reserve(non_ref_dds.size());
        for (const double d : non_ref_dds) devs.push_back(std::abs(d - median_dd));
        const double disp = 1.4826 * computeMedian(devs);
        disp_out = disp;

        // --- Effective empirical scale (empirical-Bayes, pool-arm shape) ----
        // The cold prior is the physical floor expressed as a scale
        // (floor_frac·λ_IF / k), not this epoch's own dispersion.  Shrinking
        // toward the current epoch would let the event under test set its own
        // scale: on the first candidate the gate would reduce to "|median| >
        // k·(this epoch's scatter)", which a genuine multi-satellite station
        // event cannot pass (the station re-lock case the spec's R4 protects).
        // With this prior and no empirical samples the gate reduces to the
        // legacy physical floor, and the empirical arm takes over as samples
        // accumulate.  Nothing here comes from the stochastic model (project
        // rule 4).
        const double w = static_cast<double>(cs.ref_scale_count)
                       / (static_cast<double>(cs.ref_scale_count) + ref_gate_.shrink_kappa);
        double s_eff = w * cs.ref_scale_ewma + (1.0 - w) * cold_prior;
        if (!(s_eff > 0.0)) s_eff = cold_prior;

        // Fold this epoch's dispersion in.  This runs on every epoch with a
        // usable family, not only on epochs where the legacy condition fired, so
        // the scale is a quiet-epoch noise reference and not a statistic
        // conditioned on a large median (which would let a persistent drift
        // raise its own acceptance threshold until a real slip is missed).
        // Folding is also safe on a real slip: a common step leaves the
        // dispersion at the noise level, so the event under test cannot inflate
        // the scale, as folding the median would.  The sample count advances by
        // the family size n.
        //
        // Under elev_norm (§6.14.6 item 2) the folded quantity is the
        // zenith-equivalent dispersion disp·⟨sin el⟩.  `disp` is the family's
        // scatter at the elevations present this epoch, so folding it raw makes
        // the EWMA a running mixture over a changing geometry, held in
        // ConstellationState, which outlives both that geometry and the
        // reference satellite.  Normalized, it is a stationary per-station
        // receiver-noise scale, and evaluateRefGate rescales it to the
        // elevations under test.
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
        // s_eff is still mostly the cold prior floor_frac·λ_IF/k, so k·s_eff stays
        // close to phys and the hardened gate reduces to the legacy fixed bar (3σ)
        // in the epochs where it has no basis to decide.  Those are also the
        // epochs where carry_warm is false, so a fire is only logged as
        // `diagnostic` and refloats nothing.  Abstaining asserts nothing about the
        // scale (unlike the rejected "shrink toward this epoch's dispersion", which
        // suppressed a genuine station re-lock); it declines to decide until the
        // empirical arm is informative.  MW/LLI still screen the window.
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

        // --- (1) Threshold, widened for a thin family (R3) ------------------
        // Under elev_norm (§6.14.6 item 2) s_eff is a zenith-equivalent scale, and
        // the bar is rebuilt as the sigma of the statistic under test, the median
        // of the family, instead of k times the family's raw scatter:
        //
        //   σ_ref = s_z / sin(el_ref)                 the common term.  It is in
        //           every member (dd = ΔOMC(sat) − ΔOMC(ref)), so no family size
        //           averages it away: σ(median) measures flat at 17.3–18.4 mm
        //           for n = 3…10, and this term is 88 % of its variance.
        //   σ_fam = 1.2533 · s_raw / √n               the part that averages
        //           down: the asymptotic SD of a median of n iid samples.
        //
        // On the 24 h bed, s_z ≈ 12.8 mm at el_ref ≈ 62° gives σ_ref 14.5 mm
        // (measured station-local common term 14.83 mm) and σ_fam 10.1 mm, hypot
        // 17.7 mm, against a measured σ(median) of 17.70 mm.  The flat k·s_eff
        // form does not follow el_ref or n; this one does.
        const bool   thin = (static_cast<int>(n) < ref_gate_.min_n);
        double scale = s_eff;
        if (ref_gate_.elev_norm) {
            const double s_raw   = (sin_bar > 0.0) ? (s_eff / sin_bar) : s_eff;
            const double sig_ref = (sin_ref > 0.0) ? (s_eff / sin_ref) : s_eff;
            const double sig_fam = 1.2533 * s_raw / std::sqrt(static_cast<double>(n));
            scale = std::hypot(sig_ref, sig_fam);
        }
        // The physical floor is not elevation-normalized in either mode: a slip is
        // a slip at any elevation (slip-QC lesson 7).  Only the empirical arm is.
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

        // --- (2) Coherence: the family must agree on the step (R2) ----------
        // A coherent family (small scatter about a large median) is the only
        // signature that identifies the reference as the satellite that slipped,
        // and the only case in which rebasing prev_grad by the median is
        // justified.
        RefGateVerdict kind = RefGateVerdict::FireRebase;
        if (!(disp <= ref_gate_.coherence * amed)) {
            // Incoherent.  Two different situations land here, and treating them
            // alike loses a real event (a station re-lock, the case the spec's R4
            // protects):
            //
            //   - Drift: a sub-slip trend on a thin or quiet family.  The median
            //     is modest and no individual double difference is slip-sized.
            //     Suppress.
            //   - Station event: a multi-satellite re-lock.  Many individual DDs
            //     are independently slip-sized, so the median is a mixture, not a
            //     common step.  These DDs cannot tell whether the reference itself
            //     slipped; under a zero-q carry the conservative action is to
            //     reset it (standing invariant #1: a stiff prior must not carry a
            //     value across a jump), without claiming the median as its slip
            //     magnitude.  Fire, do not rebase.
            //
            // "Independently slip-sized" means larger than a full IF cycle
            // (λ_IF), not the half-cycle candidate floor: a sub-slip drift can sit
            // just above phys on every member and would otherwise be promoted to a
            // station event, which is the false family this card removes.  At
            // least max(2, ⌈n/2⌉) members must exceed it.
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
        // Not implemented: spec §2's third candidate ("if the new reference
        // slips at once as well, the common mode is station-local; hand it to the
        // pool/RAIM layer").  That test cannot be reached at this seam, for two
        // structural reasons, and a counter for it would stay at zero and read
        // as "no station-local events occurred" (standing invariant #4):
        //   (a) a receiver-level common mode cancels in the between-satellite
        //       single difference, so it never reaches the non-reference double
        //       differences this gate evaluates; and
        //   (b) the only epoch the test could fire on is the forced re-election
        //       epoch, where ref_changed is true and processDoubleDifferences
        //       applies the median-offset rebase, which removes that common mode
        //       before the gate runs.
        // tests/test_if_phase_slip_detector.cpp pins (b)
        // (ReferenceHandoverDoesNotManufactureASlip).  Station-local modes belong
        // to the pool/RAIM layer; the escalation below is what R2 requires.
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

    // The steps D1 to D7 below share one ChainWork (slip/IfPhaseChainWork.hpp);
    // processDoubleDifferences calls them in this order:
    //   D1  indexChainSatellites       the satellite lookup and the reference row
    //   D2  rebaseOnReferenceChange    the median offset after a reference change
    //   D3  formChainDoubleDifferences the between-epoch double differences
    //   D4  applyGeometryCorrection    STAGED (IfPhaseStagedUserPath.cpp)
    //   D5  runReferenceArm            the chain's own reference arm (applyRefFire)
    //   D6  inheritReferenceVerdict    the adopted-reference inheritance path
    //   D7  flagNonReferenceOutliers   STAGED (IfPhaseStagedUserPath.cpp)

    IfPhaseSlipDetector::ChainResult IfPhaseSlipDetector::processDoubleDifferences(
        const ChainEpoch& chain, ConstellationState& cs, RefArmPlan plan) {
        ChainWork w(chain, cs, log_manager_.getLogger("if_slip_detector"));
        const auto& sys_str = chain.sys_str;
        auto& run_ref_arm = plan.run_ref_arm;
        auto& inherit = plan.inherit;

        // Second lock (§22.6): an additional chain may run the arm only through
        // the empirical gate.  With ref_gate_.enable off, the legacy candidate
        // condition |median| > floor_frac·λ_IF is the decision, and on the wide
        // combinations noise meets it (§14.10.3): on (L2W,L5Q) the IF
        // coefficients are 12.26 / −11.26 (noise amplification 16.6) against
        // λ_IF = 0.1247 m, giving 5 351 candidates and 64 fires on a quiet
        // 70-epoch null.  The loader already disarms if_phase_td_ref_arm_all
        // without the gate; this check enforces the same rule in the detector.
        const bool extra_chain = (sys_str.find('|') != std::string::npos);
        if (extra_chain && !ref_gate_.enable) {
            run_ref_arm = false;
            inherit     = nullptr;
        }
        // One gating definition, set by detect(): position uncertainty or model
        // nuisance.  Re-deriving it here from position_sigma_3d would omit the
        // model-nuisance gate (§6.14.6 item 3) and let the reference gate fire
        // through a closed gate.
        const bool pos_gated = epoch_gated_;

        indexChainSatellites(w);                       // D1
        if (!w.ref_data) return std::move(w.result);

        rebaseOnReferenceChange(w);                    // D2
        formChainDoubleDifferences(w);                 // D3
        applyGeometryCorrection(w);                    // D4  STAGED: a pinned station has delta_pos 0

        if (pos_gated) {
            // Return geometry-corrected DDs for tracing/caching but suppress slip flagging
            return std::move(w.result);
        }

        // --- Reference satellite slip check (runs regardless of all_finite) ---
        // By default there is one reference arm per (station, constellation)
        // (§14.10.4).  The additional chains of `if_phase_td_pairs = all` share
        // the first chain's reference satellite where present and do not re-run
        // its test.  Re-running it is not a pure addition: it adds tests on other
        // iono-free combinations of the same arcs, and the wide ones are
        // ill-conditioned.  (L2W,L5Q) has IF coefficients 12.26 / −11.26 (noise
        // amplification 16.6, against 2.98 on (L1C,L2W)) with λ_IF = 0.1247 m, so
        // noise meets the legacy candidate condition |median(non-ref ΔΔ)| >
        // 0.5·λ_IF.  On a quiet 70-epoch, 60-station null, legacy candidates went
        // from 25 to 5 351 and fires from 1 to 64 (52 of them incoherent
        // multi-satellite station events), while the pool arm flagged nothing
        // extra, so the per-pair evaluated/flagged tally does not show the
        // false-alarm cost.  Sharing the reference keeps every chain's gradients
        // in one frame and leaves the screening where it was.
        //
        // D5 below runs the chain's own arm; D6 applies an inherited verdict.  Both
        // act through applyRefFire, so the two responses stay identical.
        if (run_ref_arm) runReferenceArm(w);

        // --- Adopted-reference inheritance path (§22.6) ----------------------
        // An additional chain that adopted the first chain's reference does not
        // run the arm (its reference's own DD is identically 0).  Without this
        // path the first chain's fire does not reach it: the step stays on every
        // non-reference DD of this chain, and the pooled test attributes it to
        // arcs that did not slip (fan-out, §22.6).
        //
        // The verdict is inherited, never re-derived: re-testing would bring back
        // the §14.10.3 false alarms on a wide combination.  The magnitude is this
        // chain's own median of its own non-reference DDs, because the step in
        // metres is per combination (the same integer slip moves each IF pair by
        // its own coefficients); only the question "did this satellite step?" is
        // shared.  Runs only when the verdict fired and names this chain's
        // current reference, and needs n ≥ 2 finite non-reference DDs.
        if (!run_ref_arm && inherit && inherit->fired
            && !inherit->satellite.empty()
            && inherit->satellite == cs.ref_satellite) {
            inheritReferenceVerdict(w, inherit);
        }

        // --- Non-reference outlier detection ---
        // Deferred mode (network producer): the driver applies the per-satellite
        // outlier flag against a network-pooled per-signal-pair MAD, so it is
        // skipped here.  The between-epoch double differences are still computed,
        // cached and carried forward, and the reference-slip check above still runs.
        if (!defer_flagging_) flagNonReferenceOutliers(w);   // D7  STAGED: the network defers

        return std::move(w.result);
    }

    void IfPhaseSlipDetector::indexChainSatellites(ChainWork& w) const {
        const auto& sat_vec = w.chain.sat_vec;
        const auto& cs = w.cs;
        auto& sat_map = w.sat_map;
        auto& ref_data = w.ref_data;

    // Build sat lookup
    for (const auto& sd : sat_vec) sat_map[sd.satellite] = &sd;

    // Find reference SatData
    ref_data = sat_map.count(cs.ref_satellite) ? sat_map.at(cs.ref_satellite) : nullptr;
    }

    void IfPhaseSlipDetector::rebaseOnReferenceChange(ChainWork& w) const {
        const auto& logger = w.logger;
        const auto& sys_str = w.chain.sys_str;
        const auto& grad_phi_if = w.chain.grad_phi_if;
        const auto& ref_changed = w.chain.ref_changed;
        auto& cs = w.cs;

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
    }

    void IfPhaseSlipDetector::formChainDoubleDifferences(ChainWork& w) {
        const auto& grad_phi_if = w.chain.grad_phi_if;
        const auto& cs = w.cs;
        auto& delta_grad = w.result.delta_grad;
        auto& all_finite = w.all_finite;

        // --- Compute between-epoch double differences ---
        formDoubleDifferences(grad_phi_if, cs, delta_grad, all_finite);
        for (const auto& [sat, dd] : delta_grad)
            if (!std::isfinite(dd)) { all_finite = false; break; }
    }

    void IfPhaseSlipDetector::formDoubleDifferences(
        const std::map<std::string, double>& grad_phi_if, const ConstellationState& cs,
        std::map<std::string, double>& delta_grad, bool& all_finite) {
        for (const auto& [sat, g] : grad_phi_if) {
            auto prev_it = cs.prev_grad.find(sat);
            if (prev_it != cs.prev_grad.end()) {
                delta_grad[sat] = g - prev_it->second;
            } else {
                delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();
                all_finite = false;
            }
        }
    }

    // Collect finite non-reference double differences.  If their median is
    // large, the reference satellite slipped, not the non-reference ones.
    // This runs before individual outlier detection so that the common-mode
    // reference slip is removed first.
    void IfPhaseSlipDetector::runReferenceArm(ChainWork& w) {
        const auto& sys_str = w.chain.sys_str;
        const auto& lambda_if = w.chain.lambda_if;
        auto& cs = w.cs;
        const auto& sat_map = w.sat_map;
        const auto& ref_data = w.ref_data;
        const auto& delta_grad = w.result.delta_grad;
        auto& outcome = w.result.outcome;

        std::vector<double> non_ref_dds;
        // Geometry for §6.14.6 item 2: the mean sin(elevation) of the family
        // that forms the median, and sin(el) of the reference, whose own noise
        // is the median's dominant term.  Both are clamped so a horizon arc
        // cannot blow up 1/sin(el).  Computed always (a few flops) but read
        // only under ref_gate_.elev_norm, so with it off the output is
        // byte-identical.
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
            // the reference arm could screen this station-epoch-chain.  Counted
            // whether or not the hardened gate is armed, because the legacy
            // condition below runs in both modes.  Its denominator is not the
            // pool arm's per-(station, sat, pair) sample count, which is why
            // T1r is a separate tier from T1.
            ++ref_gate_counters_.gate_opportunities;
            // The same tier split per gradient chain (§22.6).  The aggregate
            // above sums every chain into one number, so it cannot show
            // whether the additional chains' arm is a flood or a backstop.
            auto& rc_own = ref_arm_chain_counters_[sys_str];
            ++rc_own.opportunities;
            const double median_dd = computeMedian(non_ref_dds);

            // The legacy condition is the candidate filter; when the hardened
            // gate is armed it then decides (empirical scale, coherence, thin
            // family, persistence).  With the gate disabled the legacy
            // condition is the decision, and the output is byte-identical
            //.
            RefGateVerdict verdict =
                (std::abs(median_dd) > ref_gate_.floor_frac * lambda_if)
                    ? RefGateVerdict::FireRebase
                    : RefGateVerdict::Suppress;
            if (verdict != RefGateVerdict::Suppress) ++rc_own.candidates;
            if (ref_gate_.enable) {
                // The dispersion scale advances on every epoch with a usable
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

            if (ref_fire) applyRefFire(w, median_dd, verdict, non_ref_dds.size(), false);

            // Publish this chain's verdict so the additional chains that
            // adopted its reference can inherit it (§22.6).  Recorded whether or
            // not it fired, so the inheritance path can tell "the arm ran and
            // found nothing" from "the arm did not run".
            outcome.satellite = cs.ref_satellite;
            outcome.verdict   = verdict;
            outcome.fired     = ref_fire;
        }
    }

    // applyRefFire is the action of a reference-arm fire (§22.6), shared by
    // the chain's own arm and by the adopted-reference inheritance path below
    // so the two stay identical.  `inherited` changes only the telemetry and
    // the log text: the reset key, the rebase and the trace record are the
    // same response, because the claim acted on ("this chain's reference
    // stepped") is the same whichever arm established it.
    void IfPhaseSlipDetector::applyRefFire(ChainWork& w, double median_dd,
                                           RefGateVerdict verdict, std::size_t n_finite,
                                           bool inherited) {
        const auto& logger = w.logger;
        const auto& sys_str = w.chain.sys_str;
        const auto& grad_phi_if = w.chain.grad_phi_if;
        const auto& lambda_if = w.chain.lambda_if;
        const auto& position_sigma_3d = w.chain.position_sigma_3d;
        auto& cs = w.cs;
        const auto& ref_data = w.ref_data;
        auto& delta_grad = w.result.delta_grad;
        auto& reset_keys = w.result.reset_keys;
        auto& all_finite = w.all_finite;

        auto& rc = ref_arm_chain_counters_[sys_str];
        if (inherited) ++rc.fires_inherited; else ++rc.fires_own;

        const std::string ref_amb_key = cs.ref_satellite + "_" + ref_data->phase_if_key;
        reset_keys.push_back(ref_amb_key);

        // An own-arm fire logs the unchanged text; only an inherited fire
        // adds a clause, so no existing log line changes under the knob.
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

        logSlipRecord(*ref_data, sys_str, 0.0, median_dd,
            cs.ref_satellite, lambda_if, "ref_slip",
            position_sigma_3d);

        // Correct the previous gradients by the median offset and recompute
        // the double differences, so that the later outlier detection (here,
        // or the network pool the driver runs on the cached DDs) sees only
        // the per-satellite residual after the reference slip is removed.
        // This prevents fan-out: without it the reference's own step appears
        // in every non-reference DD of the chain and is attributed to arcs
        // that did not slip (§22.6).
        //
        // Skipped for a FireNoRebase verdict.  The rebase asserts
        // that the reference slipped by median_dd, which holds only for a
        // coherent common step.  On an incoherent multi-satellite station
        // event the median is a mixture, and subtracting it from every other
        // satellite's history would inject a false step into clean arcs and
        // hide the real per-satellite jumps from the network pool arm
        // downstream.  The reference arc is still reset (above); only the
        // magnitude claim is dropped.
        if (verdict != RefGateVerdict::FireRebase) return;

        // Count the fan-out rows this rebase clears: non-reference DDs above
        // the chain's physical floor before it and below the floor after.  This
        // is the on-path count of rows that would otherwise have been
        // attributed to a clean arc (invariant #4); it counts floor crossings
        // and makes no claim about what the network pool would have flagged.
        const double floor_m = ref_gate_.floor_frac * lambda_if;
        for (const auto& [sat, dd] : delta_grad)
            if (sat != cs.ref_satellite && std::isfinite(dd)
                && std::abs(dd) > floor_m && std::abs(dd - median_dd) <= floor_m)
                ++rc.fanout_suppressed;

        for (auto& [sat, prev_g] : cs.prev_grad)
            if (sat != cs.ref_satellite) prev_g += median_dd;

        delta_grad.clear();
        all_finite = true;
        formDoubleDifferences(grad_phi_if, cs, delta_grad, all_finite);
    }

    void IfPhaseSlipDetector::inheritReferenceVerdict(ChainWork& w,
                                                      const RefArmOutcome* inherit) {
        const auto& sys_str = w.chain.sys_str;
        const auto& lambda_if = w.chain.lambda_if;
        const auto& cs = w.cs;
        const auto& delta_grad = w.result.delta_grad;

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
            applyRefFire(w, median_dd, inherit->verdict, non_ref_dds.size(), true);
        }
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
        last_if_jumps_.clear();
        last_if_jumps_all_.clear();
        // Monotonic detect() index: the unit of the reference gate's persistence
        // run length.  Incremented on every call (inert while the gate
        // is disabled), so it does not depend on which constellations were
        // processed.
        ++epoch_counter_;

        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        const bool pos_gated = gateEpoch(position_sigma_3d, model_nuisance_m);  // STAGED gates

        auto by_constellation = groupByConstellation(observations);

        // One iteration per gradient chain, not per constellation (§14.10).  With
        // the pair width at `first` there is one chain per system and `sys_str` is
        // the bare system string, so everything below, including every log line
        // (each names the chain), is unchanged from the single-pair text.  With it
        // at `all` the additional chains are named "GPS|L1C_L5Q" and keep their
        // own prev_grad and reset keys; each adopts the first chain's reference
        // where that satellite is present and elects its own otherwise.
        //
        // first_ref_by_sys holds the first chain's elected reference per system
        // (§14.10.4).  std::map orders a string before any string it is a prefix
        // of, so "GPS" precedes "GPS|…": the first chain of a system is processed
        // before its additional chains and this map is filled when they read it.
        std::map<std::string, std::string> first_ref_by_sys;
        // The first chain's reference-arm verdict this epoch, for the additional
        // chains that adopted its reference (§22.6).  The same ordering argument
        // applies.
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

            // Reference selection (§14.10.4).  An additional chain adopts the
            // first chain's reference whenever that satellite is present here,
            // so all of a satellite's pairs share one between-satellite frame and
            // the reference arm (by default run on the first chain only) still
            // covers the reference.  The chain elects its own reference only when
            // its satellite set does not contain the first chain's reference, e.g.
            // when that reference tracks L1/L2 but not L5.
            bool ref_changed;
            bool adopted_ref = false;        // which of the two paths ran (§22.6)
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

            // The two backstops for a slip on an additional chain's reference
            // (§22.6), both off unless ref_arm_all_ is set:
            //   own-elected  ⇒ run the arm on this chain through the empirical
            //                  gate, on this chain's own cs.ref_scale_* state and
            //                  its own λ_IF.  Requires ref_gate_.enable: on a wide
            //                  combination noise meets the legacy 0.5·λ_IF
            //                  candidate condition (§14.10.3), and
            //                  processDoubleDifferences refuses the combination
            //                  a second time.
            //   adopted      ⇒ inherit the first chain's verdict for that
            //                  satellite this epoch (no second test).
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

            // The chain's epoch: the input of the double-difference step.
            const ChainEpoch chain{ sys_str, sat_vec, grad_phi_if, lambda_if, ref_changed,
                                    delta_pos, position_sigma_3d };

            // Double-difference processing (if previous epoch exists).
            // Returns geometry-corrected, reference-slip-corrected delta_grads.
            std::map<std::string, double> delta_grad;
            if (!cs.prev_grad.empty()) {
                ChainResult result = processDoubleDifferences(chain, cs,
                    RefArmPlan{ /*run_ref_arm=*/first_pair_chain || run_extra_own_arm,  // §14.10.4 / §22.6
                                /*inherit=*/inherit });
                delta_grad = std::move(result.delta_grad);
                reset_keys.insert(reset_keys.end(), result.reset_keys.begin(),
                                  result.reset_keys.end());
                if (first_pair_chain && !result.outcome.satellite.empty())
                    first_ref_arm_by_sys[sys_only] = result.outcome;
            }

            // For satellites without a previous epoch, fill NaN
            for (const auto& [sat, g] : grad_phi_if)
                if (delta_grad.find(sat) == delta_grad.end())
                    delta_grad[sat] = std::numeric_limits<double>::quiet_NaN();

            publishChainJumps(chain, cs, delta_grad, first_pair_chain, pos_gated);

            storeChainGradients(chain, cs, reset_keys);

            // Store receiver position for geometry correction on next epoch.
            if (receiver_position.norm() > 1e-6) {
                cs.prev_position = receiver_position;
                cs.has_prev_position = true;
            }
        }

        return reset_keys;
    }

    void IfPhaseSlipDetector::publishChainJumps(const ChainEpoch& chain,
        const ConstellationState& cs, const std::map<std::string, double>& delta_grad,
        bool first_pair_chain, bool pos_gated) {
        const auto& sys_str = chain.sys_str;
        const auto& sat_vec = chain.sat_vec;
        const auto& lambda_if = chain.lambda_if;

        // Build per-satellite lookup for caching: elevation, the IF pair
        // signature (sig1/sig2), and the per-pair IF wavelength c/(f1+f2) =
        // the (1,1) slip jump, used as the physical detection floor downstream.
        std::map<std::string, const SatData*> sat_lookup;
        for (const auto& sd : sat_vec)
            sat_lookup[sd.satellite] = &sd;

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
            // A gated epoch publishes its jumps as invalid (§6.14.6 item 3).
            // The network pool arm admits any sample with `jp.valid`, so a
            // jump marked valid on a gated epoch would be thresholded by the
            // pool even though this detector suppressed its own flagging; the
            // gate has to hold for both arms.  The jumps are still cached (a
            // downstream consumer may read the value) and the gradient chain
            // still advances, so detection resumes directly after the gate.
            info.valid        = std::isfinite(dd) && !pos_gated;
            info.is_reference = (sat == cs.ref_satellite);
            // The chain this sample came from (§14.10), so a driver that flags
            // it can restart that chain alone.
            info.state_key    = sys_str;
            // The post-flag evaluation of a re-seeded gradient: the
            // sample a reset would have left out.
            info.after_reseed = (cs.reseeded.count(sat) > 0);
            // The all-pairs cache is a superset and is always filled; the
            // per-satellite cache holds the first pair only, as with the knob off.
            if (!info.sig1.empty() && !info.sig2.empty())
                last_if_jumps_all_[{ sat, info.sig1 + "_" + info.sig2 }] = info;
            if (first_pair_chain) last_if_jumps_[sat] = info;
        }
    }

    void IfPhaseSlipDetector::storeChainGradients(const ChainEpoch& chain,
        ConstellationState& cs, const std::vector<std::string>& reset_keys) {
        const auto& grad_phi_if = chain.grad_phi_if;

        // Store gradients for next epoch (exclude reset satellites).  The
        // re-seed marks described the gradients being replaced, so they go too.
        cs.prev_grad.clear();
        cs.reseeded.clear();
        for (const auto& [sat, g] : grad_phi_if) {
            bool was_reset = false;
            for (const auto& key : reset_keys)
                if (key.find(sat + "_") == 0) { was_reset = true; break; }
            if (!was_reset) {
                cs.prev_grad[sat] = g;
            }
        }
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
                cs.reseeded.erase(satellite);
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
        // Scoped to one gradient chain (§14.10): a flag on the (L1C,L5Q) pair
        // leaves the (L1C,L2W) chain, which did not flag, intact.  Takes the same
        // two actions as resetSatellite, on one entry instead of every entry.
        auto sit = constellation_state_.find(state_key);
        if (sit == constellation_state_.end()) return;
        auto& cs = sit->second;
        auto logger = log_manager_.getLogger("if_slip_detector");
        auto it = cs.prev_grad.find(satellite);
        if (it != cs.prev_grad.end()) {
            logger->debug("Cleared IF phase history for {} in chain {}", satellite, state_key);
            cs.prev_grad.erase(it);
            cs.reseeded.erase(satellite);
        }
        if (cs.ref_satellite == satellite) {
            logger->debug("Reference satellite {} for chain {} was reset, will reselect next epoch",
                satellite, state_key);
            cs.ref_satellite.clear();
            cs.ref_elevation = 0.0;
        }
    }

    // =================================================================
    // External re-seed (slip-ladder defect 2)
    // =================================================================

    bool IfPhaseSlipDetector::reseedInChain(const std::string& state_key,
        ConstellationState& cs, const std::string& satellite) {
        // Between the flag epoch's detect() and the next one, prev_grad holds the
        // flag epoch's own single difference for every satellite it saw (in
        // deferred mode only a reference-arm key excludes one).  That gradient
        // already contains the step, so it is kept as it stands: nothing is
        // rewritten, the entry is only marked so the next sample can say it is the
        // post-flag evaluation.  The reference handling is the reset's.
        auto logger = log_manager_.getLogger("if_slip_detector");
        const bool kept = (cs.prev_grad.count(satellite) > 0);
        if (kept) {
            cs.reseeded.insert(satellite);
            logger->debug("Re-seeded IF phase history for {} in chain {} from the flag epoch",
                satellite, state_key);
        }
        if (cs.ref_satellite == satellite) {
            logger->debug("Reference satellite {} for chain {} was re-seeded, will reselect "
                "next epoch", satellite, state_key);
            cs.ref_satellite.clear();
            cs.ref_elevation = 0.0;
        }
        return kept;
    }

    bool IfPhaseSlipDetector::reseedSatellite(const std::string& satellite) {
        bool kept = false;
        for (auto& [key, cs] : constellation_state_)
            kept = reseedInChain(key, cs, satellite) || kept;
        return kept;
    }

    bool IfPhaseSlipDetector::reseedSatelliteInState(const std::string& state_key,
        const std::string& satellite) {
        auto sit = constellation_state_.find(state_key);
        if (sit == constellation_state_.end()) return false;
        return reseedInChain(state_key, sit->second, satellite);
    }

} // namespace GnssProcessor

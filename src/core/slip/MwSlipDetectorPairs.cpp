/**
 * @file MwSlipDetectorPairs.cpp
 * @brief The MW detector's signal pairs and the MW combination.
 *
 * Which (phase, code) pairs an observation offers (the ionosphere-free labels of the
 * combined path, the band pairs of the uncombined path, under the two MW scope
 * knobs), the wide-lane wavelength, the loss-of-lock test, the MW observable itself
 * and the satellite MW bias from the BIA product.  No state: every function here
 * reads the observation and the configuration only.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class);
 * card UOM step U5m-c moved these bodies here unchanged (scripts/uom_u5m_verify.py
 * proves the move against the single source file of f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include "common/Constants.hpp"
#include "common/SignalBand.hpp"
#include "model/PppModel.hpp"  // getAllowedBandDigits
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // Signal / frequency helpers
    // =================================================================

    std::vector<MwSlipDetector::IfPair>
    MwSlipDetector::collectIonoFreePairs(const PppObservation& obs) const {
        // Build maps: band-pair key -> (phase label, code label).
        // An IF label like "L1W_L5Q" has band-pair key "1_5".
        std::map<std::string, std::string> phase_by_bands;
        std::map<std::string, std::string> code_by_bands;
        for (const auto& [k, v] : obs.observed) {
            if (k.size() < 2 || k.find('_') == std::string::npos) continue;
            const size_t us = k.find('_');
            if (us < 2 || us + 2 >= k.size()) continue;
            // Extract band digits from each half: e.g. "L1W" -> '1', "L5Q" -> '5'.
            const char b1 = k[1];
            const char b2 = k[us + 2];
            if (b1 < '1' || b1 > '9' || b2 < '1' || b2 > '9') continue;
            const std::string bkey = std::string(1, b1) + "_" + std::string(1, b2);
            if (k[0] == 'L') phase_by_bands[bkey] = k;
            else if (k[0] == 'C') code_by_bands[bkey] = k;
        }
        std::vector<IfPair> result;
        for (const auto& [bkey, pkey] : phase_by_bands) {
            auto it = code_by_bands.find(bkey);
            if (it != code_by_bands.end())
                result.push_back({pkey, it->second});
        }
        return result;
    }

    bool MwSlipDetector::splitIfLabel(const std::string& label,
        std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    double MwSlipDetector::computeWideLaneWavelengthMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2) const {

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) {
            log_manager_.getLogger("mw_slip_detector")->debug(
                "Invalid frequencies for {} {}: sig1={} f1={:.3f} MHz, sig2={} f2={:.3f} MHz",
                obs.satellite, PppUtils::getSystemString(obs.system),
                sig1, f1 / 1e6, sig2, f2 / 1e6);
            return 0.0;
        }

        const double df = std::abs(f1 - f2);
        if (!(std::isfinite(df) && df > 0.0)) return 0.0;

        const double lambda_wl = Constants::SPEED_OF_LIGHT / df;
        return (std::isfinite(lambda_wl) && lambda_wl > 0.0) ? lambda_wl : 0.0;
    }

    bool MwSlipDetector::hasAnyPhaseLockLoss(const PppObservation& obs) const {
        for (const auto& [type, lli] : obs.lli_flags)
            if (!type.empty() && type[0] == 'L' && lli != 0) return true;
        return false;
    }

    std::vector<MwSlipDetector::UncombinedPair>
    MwSlipDetector::collectUncombinedPairs(const PppObservation& obs) const {
        struct BandSignals {
            std::vector<std::string> codes;
            std::vector<std::string> phases;
        };
        std::map<char, BandSignals> bands;

        // Two independent knobs control which observables form MW pairs:
        //
        //  mw_frequency_mode → band scope:
        //    Mirror (default): obs.observed, exactly the bands the KF
        //      processes, keeping the detector aligned with the KF.
        //    Dual / Triple: obs.raw_observed filtered to the band set
        //      that @c getAllowedBandDigits returns for this constellation
        //      and FrequencyMode, independent of the KF's band scope.
        //    All:           obs.raw_observed unfiltered.
        //
        //  mw_channel_mode → within-band tracking-mode multiplicity:
        //    Mirror (default): inherits the KF's @c ChannelMode.
        //    Single: one code and one phase per band (e.g. L1W only on
        //      GPS L1), picked by the preference below.  Matches the
        //      legacy behaviour.
        //    All:    every tracking-mode channel in the band (e.g. both
        //      L1C and L1W on GPS L1), enabling additional cross-channel
        //      MW pairs for slip cross-consistency.
        const MwFrequencyMode mw_freq_raw = config_.getMwFrequencyMode();
        const bool use_raw       = (mw_freq_raw != MwFrequencyMode::Mirror);
        const bool all_channels  = (config_.getResolvedMwChannelMode() == ChannelMode::All);
        const auto& source       = use_raw ? obs.raw_observed : obs.observed;

        // Only Dual/Triple need a band-digit filter applied to raw_observed.
        // Mirror reads the already-band-filtered obs.observed, and All is
        // unfiltered by definition.  Map the constellation prefix once.
        std::set<char> allowed_digits;  // empty means "no filter"
        if (mw_freq_raw == MwFrequencyMode::Dual || mw_freq_raw == MwFrequencyMode::Triple) {
            auto consName = [](char c) -> std::string {
                switch (c) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return {};
                }
            };
            const std::string cons = obs.satellite.empty() ? "" : consName(obs.satellite[0]);
            const FrequencyMode fm =
                (mw_freq_raw == MwFrequencyMode::Dual) ? FrequencyMode::Dual
                                                       : FrequencyMode::Triple;
            allowed_digits = getAllowedBandDigits(cons, fm);
        }

        // In single-channel mode the bucket holds one key per band, picked
        // by a two-level preference:
        //   1. KF-chosen (key is in obs.observed): keeps MW and KF aligned
        //      on which channel represents the band.  Without this,
        //      iterating raw_observed alphabetically would pick L1C even
        //      when the KF estimates L1W.
        //   2. Canonical tracking-attribute priority via
        //      @c signalAttributePriority: picks the best channel for
        //      bands the KF doesn't track (e.g. L5X over L5I/L5Q for GPS
        //      L5 under signal_mode=dual).
        // This mirrors the same preference logic the KF uses to populate
        // obs.observed in @c PppModel::buildFrequencyBandMap.
        // A companion code row is not KF-chosen for this detector's purposes
        // (card SCC, R4).  With `network.second_code_channel` the KF estimates a
        // second band-1 code, so an unguarded `obs.observed` test would promote C1W
        // over C1C at every station that reports both, moving the detector's code for
        // the band and with it every MW arc, every threshold and every slip event the
        // run reports.  The rule is that the knob leaves the detector unchanged, so a
        // companion reads as it did without the knob: absent from obs.observed.
        // `companion_codes` is empty on every unarmed posture, so this costs one
        // lookup on an empty set and the verdict is byte-identical.  A fallback code
        // is deliberately not in that set and stays eligible as before.
        // The same rule applies to a second phase (card SID phase 6a, R4).  With
        // `network.second_phase_channel` the KF estimates a second band-1 phase, and
        // GPS L1 attribute priority puts 'W' ahead of 'C', so an unguarded test would
        // promote L1W over L1C in the band's phase bucket at every station that
        // reports both, moving the detector's phase for the band and with it every
        // MW arc and every slip event.  `second_phases` is empty on every unarmed
        // posture, so this costs one lookup on an empty set and the verdict is
        // byte-identical.
        auto isKfChosen = [&obs](const std::string& key) {
            if (obs.companion_codes.count(key)) return false;
            if (obs.second_phases.count(key))   return false;
            return obs.observed.find(key) != obs.observed.end();
        };
        auto priorityOf = [&obs](const std::string& key) -> int {
            if (key.size() < 3) return std::numeric_limits<int>::max();
            const auto cls = classifySignalForSat(obs.satellite, key);
            if (cls.band == FrequencyBand::Unknown)
                return std::numeric_limits<int>::max();
            // signalAttributePriority switches on band only; the
            // constellation string is unused but the API takes it for
            // forward compatibility, so the sat id is passed as a stable hint.
            return signalAttributePriority(obs.satellite, cls.band, cls.attribute);
        };
        auto preferNew = [&](const std::string& incumbent,
                              const std::string& candidate) {
            const bool inc_kf = isKfChosen(incumbent);
            const bool cand_kf = isKfChosen(candidate);
            if (cand_kf != inc_kf) return cand_kf;   // KF-chosen always wins.
            return priorityOf(candidate) < priorityOf(incumbent);
        };

        for (const auto& [key, val] : source) {
            if (key.size() < 2) continue;
            const char prefix = key[0];
            if (prefix != 'C' && prefix != 'L') continue;
            if (key.find('_') != std::string::npos) continue;
            // Second half of the companion-code guard (card SCC, R4).  When `source`
            // is obs.observed (mw_frequency_mode Mirror) a companion key would enter a
            // bucket that did not hold it without the knob; skip it, so Mirror sees the
            // same key set as unarmed.  When `source` is obs.raw_observed the key was
            // always there and dropping it would change the unarmed behaviour, so it
            // stays, and the isKfChosen guard above keeps it from winning the band.
            // With the set empty (knob off) both paths are byte-identical.
            if (!use_raw && obs.companion_codes.count(key)) continue;
            // Second half of the second-phase guard (card SID phase 6a, R4), with one
            // deliberate difference from the line above.  In a single-channel bucket
            // (mw_channel_mode mirror or single) the second phase must be invisible, so
            // the detector keeps the key set it had unarmed.  In all-channel mode the
            // bucket asks for every tracking-mode channel of the band, which is the
            // mode the card's G1c arm measures, so the second phase enters and the
            // cross-band pair loop below forms its own pairs with it.  Measured, not
            // assumed: with mw_frequency_mode Mirror the card SCC line above has already
            // removed the companion code, so `matchChannel` pairs an L1W phase with the
            // latched C1C.  That puts the receiver's C1C-minus-C1W inter-signal delay into
            // the MW value as a constant per arc, which the arc's own running reference
            // absorbs; it is a level offset, not a slope, so it does not by itself
            // produce jump detections.  With the set empty (knob off) the result is
            // byte-identical.
            if (!use_raw && !all_channels && obs.second_phases.count(key)) continue;
            const char band = key[1];
            if (band < '1' || band > '9') continue;
            // Band-scope filter (empty set = "no filter").
            if (!allowed_digits.empty() && allowed_digits.count(band) == 0)
                continue;

            auto& bs = bands[band];
            auto& bucket = (prefix == 'C') ? bs.codes : bs.phases;
            if (all_channels) {
                bucket.push_back(key);
            } else if (bucket.empty()) {
                bucket.push_back(key);
            } else if (preferNew(bucket.front(), key)) {
                bucket.front() = key;
            }
        }

        std::vector<char> band_ids;
        for (const auto& [b, bs] : bands)
            if (!bs.codes.empty() && !bs.phases.empty())
                band_ids.push_back(b);

        // Sort band_ids by carrier frequency descending so every pair
        // built below has f1 > f2.  The MW sign convention requires
        // signed Δf for slip-integer attribution to come out right
        // (computeMwMeters uses the signed f_diff; the wavelength
        // helper uses |f_diff|).  Without this normalization the
        // lexical map-iteration order of band characters produces
        // pairs whose f1 < f2 for constellations where the band-letter
        // sort doesn't match the frequency sort, notably Galileo
        // (bands 1,5,6,7,8 with frequencies 1575/1176/1278/1207/1192
        // MHz; the (5,*) pairs come out with f1 < f2), QZSS, and
        // BeiDou.  The downstream culprit-attribution sign flip in
        // CycleSlipManager::computeCulpritSlipIntegers assumes the
        // signed-Δf convention, so a wrong-order pair contributes a
        // sign-flipped slip integer to the cross-pair consistency
        // check, masking what would otherwise be a clean attribution
        // and forcing fall-through to reset instead of integer repair.
        //
        // Tie-break with band character so the order stays
        // deterministic across runs with identical signal sets.
        std::sort(band_ids.begin(), band_ids.end(), [&](char a, char b) {
            const auto& pa = bands.at(a).phases.front();
            const auto& pb = bands.at(b).phases.front();
            const double fa = ppp_utils_.getFrequency(pa, obs.system, obs.satellite);
            const double fb = ppp_utils_.getFrequency(pb, obs.system, obs.satellite);
            if (std::isfinite(fa) && std::isfinite(fb) && fa != fb)
                return fa > fb;
            return a < b;
        });

        // Helper: pick the code on the same RINEX-3 tracking-mode channel
        // as the phase (e.g. L1C → C1C, L1W → C1W) so the MW combination
        // sees consistent inter-channel biases on both ends.  Fall back
        // to the first code in the band if no same-channel code exists
        // (rare; happens when only one tracking mode publishes a code).
        auto matchChannel = [](const std::string& phase,
                                const std::vector<std::string>& codes) -> std::string {
            if (codes.empty()) return {};
            if (phase.size() >= 3) {
                const char ch = phase[2];
                for (const auto& c : codes) {
                    if (c.size() >= 3 && c[2] == ch) return c;
                }
            }
            return codes.front();
        };

        std::vector<UncombinedPair> pairs;
        for (size_t i = 0; i < band_ids.size(); ++i) {
            for (size_t j = i + 1; j < band_ids.size(); ++j) {
                const auto& b1 = bands[band_ids[i]];
                const auto& b2 = bands[band_ids[j]];
                for (const auto& phase1 : b1.phases) {
                    const std::string code1 = matchChannel(phase1, b1.codes);
                    if (code1.empty()) continue;
                    for (const auto& phase2 : b2.phases) {
                        const std::string code2 = matchChannel(phase2, b2.codes);
                        if (code2.empty()) continue;
                        pairs.push_back({phase1, phase2, code1, code2});
                    }
                }
            }
        }
        return pairs;
    }

    // =================================================================
    // MW computation
    // =================================================================

    double MwSlipDetector::computeMwMeters(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2,
        const std::string& code1, const std::string& code2) const {

        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        if (sig1.empty() || sig2.empty() || sig1[0] != 'L' || sig2[0] != 'L') return NaN;
        if (code1.empty() || code2.empty() || code1[0] != 'C' || code2[0] != 'C') return NaN;

        auto itL1 = obs.raw_observed.find(sig1);
        auto itL2 = obs.raw_observed.find(sig2);
        auto itP1 = obs.raw_observed.find(code1);
        auto itP2 = obs.raw_observed.find(code2);

        if (itL1 == obs.raw_observed.end() || itL2 == obs.raw_observed.end() ||
            itP1 == obs.raw_observed.end() || itP2 == obs.raw_observed.end())
            return NaN;

        const double L1_cyc = itL1->second;
        const double L2_cyc = itL2->second;
        const double P1_m   = itP1->second;
        const double P2_m   = itP2->second;

        if (!std::isfinite(L1_cyc) || !std::isfinite(L2_cyc) ||
            !std::isfinite(P1_m) || !std::isfinite(P2_m))
            return NaN;

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return NaN;

        const double f_diff = f1 - f2;
        const double f_sum  = f1 + f2;
        if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) return NaN;

        const double c = Constants::SPEED_OF_LIGHT;
        const double L1_m = L1_cyc * c / f1;
        const double L2_m = L2_cyc * c / f2;

        const double wl_phase_m = (f1 * L1_m - f2 * L2_m) / f_diff;
        const double nl_code_m  = (f1 * P1_m + f2 * P2_m) / f_sum;
        const double mw_m = wl_phase_m - nl_code_m;

        return std::isfinite(mw_m) ? mw_m : NaN;
    }

    bool MwSlipDetector::computeSatMwBias(const PppObservation& obs,
        const std::string& sig1, const std::string& sig2,
        const std::string& code1, const std::string& code2,
        double& bias_m) const {

        bias_m = 0.0;
        if (!corrections_) return false;

        // Check all four BIA entries exist
        if (!corrections_->hasBiasEntry(obs.satellite, sig1) ||
            !corrections_->hasBiasEntry(obs.satellite, sig2) ||
            !corrections_->hasBiasEntry(obs.satellite, code1) ||
            !corrections_->hasBiasEntry(obs.satellite, code2))
            return false;

        const double b_L1 = corrections_->getBiasCorrection(obs.satellite, sig1, obs.time);
        const double b_L2 = corrections_->getBiasCorrection(obs.satellite, sig2, obs.time);
        const double b_C1 = corrections_->getBiasCorrection(obs.satellite, code1, obs.time);
        const double b_C2 = corrections_->getBiasCorrection(obs.satellite, code2, obs.time);

        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);
        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) return false;

        const double f_diff = f1 - f2;
        const double f_sum  = f1 + f2;
        if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) return false;

        // b_MW = (f1*b_L1 - f2*b_L2)/(f1-f2) - (f1*b_C1 + f2*b_C2)/(f1+f2)
        const double wl_phase_bias = (f1 * b_L1 - f2 * b_L2) / f_diff;
        const double nl_code_bias  = (f1 * b_C1 + f2 * b_C2) / f_sum;
        bias_m = wl_phase_bias - nl_code_bias;

        return std::isfinite(bias_m);
    }

} // namespace GnssProcessor

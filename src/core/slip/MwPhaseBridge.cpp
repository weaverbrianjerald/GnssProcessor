/**
 * @file MwPhaseBridge.cpp
 * @brief The MW detector's tri-frequency phase bridge (network-only, owned with card NOM).
 *
 * The geometry-free WL-to-EWL phase bridge arcs (cards WL-2, WL-3, UWL-1): the
 * per-sample fold and its three reads.  Off unless the network arms
 * setBridgeEnabled; the user drivers never touch it.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class);
 * card UOM step U5m-c moved these bodies here unchanged (scripts/uom_u5m_verify.py
 * proves the move against the single source file of f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include "math/TimeUtils.hpp"
#include "common/Constants.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // Tri-frequency phase bridge (M5 card WL-2, SPEC §3.4)
    // =================================================================

    MwSlipDetector::MwBridgeMean
    MwSlipDetector::arcBridge(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2,
                              const std::string& sig5) const {
        MwBridgeMean out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        // Serve the lagged snapshot (state as of the previous epoch's end) so a same-epoch
        // consumer never re-ingests the current observations: the §3.3/§3.4 double-count
        // guard, identical to arcZdMean.
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)) {
            out.count = s.count_lag;
            return out;
        }
        const double n = static_cast<double>(s.count_lag);
        out.gf_mean_m       = s.mean_lag;
        out.var_of_mean_m2  = s.M2_lag / (n * (n - 1.0));
        out.count           = s.count_lag;
        out.lambda_wl       = s.lambda_wl;
        out.lambda_ewl      = s.lambda_ewl;
        out.iono_coeff      = s.iono_coeff;
        out.valid           = std::isfinite(out.gf_mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
    }

    MwSlipDetector::MwBridgeLast
    MwSlipDetector::arcBridgeLast(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2,
                                  const std::string& sig5) const {
        MwBridgeLast out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        out.count = s.count_lag;
        // The lagged raw GF_b sample and per-sample Welford variance: the bridge lane's
        // per-epoch increment (WL-3 §3.2; used once; the mean stays decision-side).
        if (s.count_lag < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)
            || !std::isfinite(s.prev_gf_lag))
            return out;
        out.gf_m       = s.prev_gf_lag;
        out.var_m2     = s.M2_lag / (static_cast<double>(s.count_lag) - 1.0);
        out.epoch_s    = s.prev_epoch_lag_s;
        out.lambda_wl  = s.lambda_wl;
        out.lambda_ewl = s.lambda_ewl;
        out.iono_coeff = s.iono_coeff;
        out.valid      = std::isfinite(out.gf_m) && std::isfinite(out.var_m2);
        return out;
    }

    MwSlipDetector::MwBridgeMean
    MwSlipDetector::arcBridgeCold(const std::string& sat,
                                  const std::string& sig1,
                                  const std::string& sig2,
                                  const std::string& sig5) const {
        MwBridgeMean out;
        const std::string key = sat + "_" + sig1 + "_" + sig2 + "_" + sig5;
        const auto it = bridge_stats_.find(key);
        if (it == bridge_stats_.end()) return out;
        const BridgeArcStats& s = it->second;
        out.count = s.count;
        // Unlagged serve from n ≥ 2 (the variance of the mean needs two samples), one
        // epoch earlier than the lagged arcBridge (count_lag ≥ 2 ⇔ three samples).
        // Decision-only (header contract); the lane increment stays on arcBridgeLast.
        if (s.count < 2 || !(s.lambda_wl > 0.0) || !(s.lambda_ewl > 0.0)) return out;
        const double n = static_cast<double>(s.count);
        out.gf_mean_m      = s.mean;
        out.var_of_mean_m2 = s.M2 / (n * (n - 1.0));
        out.lambda_wl      = s.lambda_wl;
        out.lambda_ewl     = s.lambda_ewl;
        out.iono_coeff     = s.iono_coeff;
        out.valid          = std::isfinite(out.gf_mean_m) && std::isfinite(out.var_of_mean_m2);
        return out;
    }

    void MwSlipDetector::updateBridgeArc(const PppObservation& obs, bool has_lli) {
        if (!bridge_enabled_ || obs.satellite.empty()) return;

        const char cons = obs.satellite.front();
        // Per-constellation lane-tier band digits (UWL-1, SPEC_uwl_card.md §3.1).
        // Empty table = the legacy hardcoded {1,2,5} scan for every constellation
        // (byte-identical; it forms no arc for Galileo, which has no band '2', so the
        // cascade is then GPS-only).  A pushed table with no entry for this
        // constellation forms no bridge arc (BDS-3 is deferred on its clock
        // divergence; the wiring logs a warning).
        char b1 = '1', b2 = '2', bt = '5', bm = '\0', bt2 = '\0';
        if (!lane_tiers_.empty()) {
            const auto tit = lane_tiers_.find(cons);
            if (tit == lane_tiers_.end()) return;
            b1 = tit->second.b1; b2 = tit->second.b2; bt = tit->second.b_top;
            bm = tit->second.b_mid; bt2 = tit->second.b_top2;
        }

        // KF-aligned single phase signal per lane-tier band: min-name over obs.observed,
        // matching the ApplicationNetwork primary(sig1<sig2 by band then name) + band-top
        // (first b_top) selection so this arc's key aligns with the caller's arcBridge query.
        // When the network pushed its channel census (@ref setBridgePreferredChannels,
        // WL-3), the preferred channel takes precedence where tracked.  Otherwise the
        // caller's census-selected primary pair would miss the min-name-keyed arc at every
        // (rx,sat) whose channels differ, and the WL decision path would lose those cycles.
        std::string s1, s2, s5, sm, st2;  // WL-pair bands (b1, b2) + top band (b_top) + optional mid (b_mid) / fallback top (b_top2)
        for (const auto& [k, v] : obs.observed) {
            if (k.size() < 2 || k[0] != 'L') continue;
            (void)v;
            std::string* dst = (k[1] == b1) ? &s1 : (k[1] == b2) ? &s2
                             : (k[1] == bt) ? &s5
                             : (bm  != '\0' && k[1] == bm)  ? &sm
                             : (bt2 != '\0' && k[1] == bt2) ? &st2 : nullptr;
            if (!dst) continue;
            if (!bridge_pref_.empty()) {
                const auto pit = bridge_pref_.find({cons, k[1]});
                if (pit != bridge_pref_.end()) {
                    if (k == pit->second) *dst = k;              // preferred channel wins
                    else if (dst->empty()) *dst = k;             // fallback until seen
                    else if (*dst != pit->second && k < *dst) *dst = k;  // min-name among fallbacks
                    continue;
                }
            }
            if (dst->empty() || k < *dst) *dst = k;
        }
        // Under a dual-top tier the fallback triple must form even when the preferred
        // band is untracked at this (rx,sat); that is what the fallback rung is for
        // (increment 5, uwl_dual_top).  Single-top tiers keep the legacy early-out
        // unchanged (bt2 == '\0' ⇒ have_top2 is false ⇒ identical flow).
        const bool have_top  = !s5.empty();
        const bool have_top2 = (bt2 != '\0') && !st2.empty();
        if (s1.empty() || s2.empty() || (!have_top && !have_top2)) return;

        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // One bridge triple (sa, sb, sc): lane A = (sa, sb), lane B = (hi, lo of (sb, sc)).
        // GF = λ_A(φ_sa−φ_sb) − λ_B(φ_hi−φ_lo) = λ_A·N_A − λ_B·N_B + iono + noise.
        // Extracted so the E6 mid hop (UWL-1, census-motivated) can feed additional
        // triples per observation: the base (b1,b2,b_top) always (byte-identical to the
        // single-triple code for the legacy and mid-off shapes), plus (b_mid,b2,b_top)
        // [hop-hi: mid lane vs top lane] and (b1,b2,b_mid) [hop-lo: WL lane vs mid lane]
        // when the constellation's tier arms a mid rung and the band is tracked.
        auto feedTriple = [&](const std::string& sa, const std::string& sb,
                              const std::string& sc) {
            const std::string key = obs.satellite + "_" + sa + "_" + sb + "_" + sc;

            // Loss-of-lock on any phase re-warms the bridge (it has no detection statistic
            // to catch a real slip, unlike MW's threshold gate, so resetting is the safe choice).
            if (has_lli) { bridge_stats_.erase(key); return; }

            // Raw cycle values (matching computeMwMeters' raw-observation convention).
            const auto i1 = obs.raw_observed.find(sa);
            const auto i2 = obs.raw_observed.find(sb);
            const auto i5 = obs.raw_observed.find(sc);
            if (i1 == obs.raw_observed.end() || i2 == obs.raw_observed.end() ||
                i5 == obs.raw_observed.end())
                return;
            const double L1c = i1->second, L2c = i2->second, L5c = i5->second;
            if (!(std::isfinite(L1c) && std::isfinite(L2c) && std::isfinite(L5c))) return;

            const double f1 = ppp_utils_.getFrequency(sa, obs.system, obs.satellite);
            const double f2 = ppp_utils_.getFrequency(sb, obs.system, obs.satellite);
            const double f5 = ppp_utils_.getFrequency(sc, obs.system, obs.satellite);
            if (!(std::isfinite(f1) && std::isfinite(f2) && std::isfinite(f5) &&
                  f1 > 0.0 && f2 > 0.0 && f5 > 0.0))
                return;
            const double df_wl  = f1 - f2;   // lane A: sa − sb (f_sa > f_sb by table order)
            // Lane-B pair (hi − lo) by actual frequency: GPS's shared band b2 is the top
            // pair's high side (L2 > L5), Galileo's is its low side (E5a < E5b/E5/E6);
            // UWL-1 §2 keeps λ_B positive either way.  For GPS this reproduces the
            // legacy f2 − f5 expression bit-exactly.
            const bool   b2_hi  = f2 > f5;
            const double df_top = b2_hi ? (f2 - f5) : (f5 - f2);
            if (std::abs(df_wl) < 1.0 || std::abs(df_top) < 1.0) return;

            const double c        = Constants::SPEED_OF_LIGHT;
            const double lam_wl   = c / df_wl;
            const double lam_ewl  = c / df_top;
            // GF iono sensitivity to the L1-referenced slant iono I₁ (the estimator's
            // iono-state frame, f_ref = GPS L1): C_iono = f_ref²·(1/(f1·f2) − 1/(f_hi·f_lo)).
            // The product is order-free, so the f2·f5 expression is generic (SPEC §3.4 v2:
            // the consumer subtracts C_iono·Σc·Î₁ to de-bias the bridge).
            const double f_ref    = Constants::GPS_L1_FREQ;
            const double iono_co  = f_ref * f_ref * (1.0 / (f1 * f2) - 1.0 / (f2 * f5));
            // Lane-A phase (m) = λ_A·(φ_sa−φ_sb); lane-B phase (m) = λ_B·(φ_hi−φ_lo).
            // GF = φ_A − φ_B is geometry-free → λ_A·N_A − λ_B·N_B + differential-iono
            // residue + phase noise; N_B = N_hi − N_lo matches the MW(hi,lo) integer.
            double gf = lam_wl * (L1c - L2c)
                      - lam_ewl * (b2_hi ? (L2c - L5c) : (L5c - L2c));
            if (!std::isfinite(gf)) return;
            // Per-sample iono de-bias (setBridgePerSampleDebias, the age-bias fix):
            // fold GF_k − C_iono·ı̃(t_k) so the accumulated statistic is a stationary
            // estimable-function realization; every downstream consumer (Welford mean,
            // prev_gf staging, gross-jump guard) then compares like frames.  A sample
            // without an estimate is skipped, never folded in a mixed frame.
            if (bridge_debias_) {
                const auto iit = bridge_iono_.find(obs.satellite);
                if (iit == bridge_iono_.end() || !std::isfinite(iit->second)) {
                    ++bridge_debias_skipped_;
                    return;
                }
                gf -= iono_co * iit->second;
            }

            BridgeArcStats& st = bridge_stats_[key];

            // Data-gap reset (same tolerance as the MW arcs).
            if (st.count > 0 && st.last_epoch_s > 0.0 &&
                (epoch_s - st.last_epoch_s) > max_gap_seconds_) {
                st = BridgeArcStats{};
            }
            // Gross-jump refloat: a lane slip the ambiguity funnel missed (≥ kBridgeJumpResetM
            // above the sub-decimetre iono residue).  Refloat rather than corrupt the slow mean.
            if (st.count > 0 && std::abs(gf - st.mean) > kBridgeJumpResetM) {
                st = BridgeArcStats{};
            }

            if (st.count == 0) {
                st.mean       = gf;
                st.M2         = 0.0;
                st.count      = 1;
                st.prev_gf    = gf;        // raw-sample staging for arcBridgeLast (WL-3)
                st.prev_epoch_s = epoch_s;
                st.last_epoch_s = epoch_s;
                st.lambda_wl  = lam_wl;
                st.lambda_ewl = lam_ewl;
                st.iono_coeff = iono_co;
                return;
            }

            // Lagged snapshot before this epoch's value folds in (arcBridge serves these).
            st.mean_lag  = st.mean;
            st.M2_lag    = st.M2;
            st.count_lag = st.count;
            // Raw-sample lag for arcBridgeLast (WL-3), with the same once-only use.
            st.prev_gf_lag      = st.prev_gf;
            st.prev_epoch_lag_s = st.prev_epoch_s;
            st.count++;
            const double eff_n = static_cast<double>(std::min<std::size_t>(st.count, kMaxHatchCount));
            const double delta = gf - st.mean;
            st.mean += delta / eff_n;
            const double delta2 = gf - st.mean;
            st.M2 += delta * delta2;
            st.prev_gf      = gf;        // raw-sample staging (WL-3)
            st.prev_epoch_s = epoch_s;
            st.last_epoch_s = epoch_s;
            st.lambda_wl  = lam_wl;
            st.lambda_ewl = lam_ewl;
            st.iono_coeff = iono_co;
        };

        if (have_top)  feedTriple(s1, s2, s5);   // base: WL lane vs preferred top lane (as before)
        if (have_top2) feedTriple(s1, s2, st2);  // WL lane vs fallback top lane (increment 5)
        if (bm != '\0' && !sm.empty()) {         // extra triples for the E6 mid hop, decision-side
            if (have_top)  feedTriple(sm, s2, s5);   // hop-hi: mid lane (b_mid,b2) vs preferred top
            if (have_top2) feedTriple(sm, s2, st2);  // hop-hi vs the fallback top (dual only)
            feedTriple(s1, s2, sm);                  // hop-lo: WL lane vs mid lane (b_mid,b2)
        }
    }

} // namespace GnssProcessor

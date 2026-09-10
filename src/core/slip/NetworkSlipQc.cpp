/**
 * @file NetworkSlipQc.cpp
 * @brief Serial slip-QC drains of the network run loop: the phase-leveling T0
 *        repair drain, the MW-detect / LLI refloat drain, and the IF-phase
 *        triple-difference driver.
 *
 * Decomposition INC-6 (2026-07-16) — bodies relocated VERBATIM from the
 * SB_SLIP block of ApplicationNetwork.cpp (see
 * docs/network_decomposition_plan.md).  The PASS-A parallel
 * scan that FILLS PassABuffers stays in the run TU until INC-7 (Assembly);
 * the SB_SLIP D0b timer bracket also stays there, wrapping the scan plus
 * these three calls.  Each function opens with a short alias block binding
 * the former runNetwork() local names (buffer slots, trace stream, event
 * forwarder) so the moved code is textually unchanged.
 */

#include "slip/NetworkSlipQc.hpp"

#include "config/GnssConfig.hpp"        // networkCycleSlip* getters
#include "filter/StateKeySchema.hpp"    // StateKeys::parseAmbiguityKey (refloat lambdas)
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch (se.time in log/event rows)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace GnssProcessor {

void networkSlipDrainT0Repairs(const PassABuffers& pa, const GnssConfig& config_,
                               const std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>& local_carry,
                               const SyncedEpoch& se,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-6 migration mechanic) — the relocated body reads the
    // PASS-A buffer under its former local name.
    const auto& sta_repairs = pa.sta_repairs;
    // Phase-leveling T0 (serial, station-ordered → deterministic), applied
    // BEFORE PASS-B reads the carry to build the prior.
    //   • REPAIR  — a confident, sane-magnitude integer (frac ≤ frac_max AND
    //     |dN| ≤ dn_max): shift the carried value continuously
    //     (value_abs += dN·λ, variance unchanged, covariance-preserving).
    //   • LEAVE   — a less-confident emitted repair (frac/magnitude reject):
    //     do nothing.  This is the data-driven choice for the BIA-less network:
    //     the float wide-lane's emitted-but-low-confidence repairs are
    //     predominantly FALSE positives (e.g. a re-initialising arc reporting
    //     dN=675 at frac≈0.27), so acting on them — REFLOAT or DROP — discards
    //     a GOOD carry and measurably degrades the products, whereas leaving
    //     them matches carry-only.  The residual lock-in risk on a RARE
    //     real-but-rejected slip is bounded by phase_leveling_var_floor_m2 and
    //     the max-gap drop, and is properly resolved by Increment 3's
    //     cross-station RAIM consensus (which CAN tell a real slip from a
    //     single-station false positive — something a per-station T0 cannot).
    //     Wiring external BIA into the network MW would make the wide-lane
    //     integer trustworthy and let this become a REFLOAT.
    // The resolver's `reset_sats` is likewise NOT acted on (BIA-less, it is a
    // liberal catch-all of tens of sats/epoch).  The pivot (stations[0]) is
    // never touched (its gauge-pinned ambiguities are Increment-4 territory).
    int n_t0_repairs = 0, n_t0_left = 0;
    std::string t0_detail;
    const double t0_frac_max = config_.networkCycleSlipTurboEditFracMax();
    const int    t0_dn_max   = config_.networkCycleSlipTurboEditDnMax();
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        for (const auto& r : sta_repairs[i]) {
            if (r.dN == 0 || !(std::isfinite(r.lambda) && r.lambda > 0.0)) continue;
            auto it = local_carry.find(std::make_tuple(i, r.satellite, r.signal));
            if (it == local_carry.end()) continue;
            const int adn = (r.dN < 0) ? -r.dN : r.dN;
            if (r.frac <= t0_frac_max && adn <= t0_dn_max) {
                it->second.value_abs += static_cast<double>(r.dN) * r.lambda;  // REPAIR
                ++n_t0_repairs;
                if (n_t0_repairs <= 12)
                    t0_detail += fmt::format("{}{}/{}/{} dN={} f={:.2f}",
                                             t0_detail.empty() ? "" : ", ",
                                             stations[i].name, r.satellite, r.signal,
                                             r.dN, r.frac);
            } else {
                ++n_t0_left;  // LEAVE (untrusted integer — likely a float-MW false positive)
                if (n_t0_left <= 6)
                    log->debug("Network[phase-leveling T0]: TOW {:.1f} LEFT low-confidence "
                               "{}/{}/{} dN={} frac={:.2f} (untrusted; carry kept)",
                               se.time.tow, stations[i].name, r.satellite, r.signal,
                               r.dN, r.frac);
            }
        }
    }
    if (n_t0_repairs > 0 || n_t0_left > 0)
        log->info("Network[phase-leveling T0]: TOW {:.1f} applied {} repair(s) [{}]; "
                  "left {} low-confidence detection(s) untouched",
                  se.time.tow, n_t0_repairs, t0_detail, n_t0_left);
}

DeltaBridgeSolve networkSlipDeltaBridgeSolve(double dn_wl_cyc, double sigma_wl_cyc,
                                             double dif_m, double sigma_if_m,
                                             double lambda_wl_m, double lambda_if_m,
                                             double max_abs_cyc, double max_sigma_cyc) {
    DeltaBridgeSolve r;
    if (!std::isfinite(dn_wl_cyc) || !std::isfinite(sigma_wl_cyc) ||
        !std::isfinite(dif_m) || !std::isfinite(sigma_if_m) ||
        !(lambda_wl_m > 0.0) || !(lambda_if_m > 0.0))
        return r;
    // Band frequencies from the two wavelengths alone (per-satellite exact —
    // GLONASS FDMA included): 1/λ_WL = (f1−f2)/c, 1/λ_IF = (f1+f2)/c.
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / lambda_wl_m + 1.0 / lambda_if_m);
    const double f2 = 0.5 * c * (1.0 / lambda_if_m - 1.0 / lambda_wl_m);
    if (!(f2 > 0.0) || !(f1 > f2)) return r;
    const double lam1 = c / f1, lam2 = c / f2;
    // δ_IF = β1·ΔN1 − β2·ΔN2 with β_j = λ_WL·f_j/(f1+f2); β1 − β2 ≡ λ_IF.
    const double beta1 = lambda_wl_m * f1 / (f1 + f2);
    // FLOAT solve (a bridge, never an integer fix):
    //   ΔN2 = (δ_IF − β1·ΔN_WL)/λ_IF;   ΔN1 = ΔN2 + ΔN_WL.
    // (The β1-times-ΔN_WL-over-λ_IF step is this kernel's analogue of the GF
    //  kernel's documented λ1-in-numerator trap — integer-injection tested.)
    const double dn2 = (dif_m - beta1 * dn_wl_cyc) / lambda_if_m;
    const double dn1 = dn2 + dn_wl_cyc;
    if (!std::isfinite(dn1) || !std::isfinite(dn2)) return r;
    if (std::abs(dn1) > max_abs_cyc || std::abs(dn2) > max_abs_cyc) return r;
    // Variance propagation (independent σ_IF [m], σ_WL [cyc]); the 1/λ_IF²
    // factor is the ~9× amplification the bridged prior must honestly carry.
    const double beta2 = beta1 - lambda_if_m;
    const double v2 = (sigma_if_m * sigma_if_m + beta1 * beta1 * sigma_wl_cyc * sigma_wl_cyc)
                      / (lambda_if_m * lambda_if_m);
    const double v1 = (sigma_if_m * sigma_if_m + beta2 * beta2 * sigma_wl_cyc * sigma_wl_cyc)
                      / (lambda_if_m * lambda_if_m);
    if (std::sqrt(v1) > max_sigma_cyc || std::sqrt(v2) > max_sigma_cyc) return r;
    r.ok = true;
    r.dn1_cyc = dn1;  r.dn2_cyc = dn2;
    r.shift1_m = lam1 * dn1;  r.shift2_m = lam2 * dn2;
    r.var1_m2 = lam1 * lam1 * v1;  r.var2_m2 = lam2 * lam2 * v2;
    return r;
}

BridgeV3Solve networkSlipBridgeV3Solve(const RelevelPend& pd, double rho_if_gf,
                                       double max_abs_cyc, double max_sigma_cyc) {
    BridgeV3Solve r;
    if (!(pd.lambda_wl > 0.0) || !(pd.lambda_if > 0.0)) return r;
    // Band frequencies from the two wavelengths alone (the v2 derivation —
    // per-satellite exact, GLONASS FDMA included).
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    if (!(f2 > 0.0) || !(f1 > f2)) return r;
    const double lam1 = c / f1, lam2 = c / f2;
    const double beta1 = pd.lambda_wl * f1 / (f1 + f2);
    const double beta2 = beta1 - pd.lambda_if;

    double dn1, dn2, v1, v2, cov12 = 0.0;
    if (pd.has_dn) {
        // E2 route: the v2 closed form, verbatim math.
        if (!std::isfinite(pd.dif_m) || !std::isfinite(pd.sigma_if_m)) return r;
        dn2 = (pd.dif_m - beta1 * pd.dn_wl) / pd.lambda_if;
        dn1 = dn2 + pd.dn_wl;
        v2  = (pd.sigma_if_m * pd.sigma_if_m
               + beta1 * beta1 * pd.sigma_wl * pd.sigma_wl)
              / (pd.lambda_if * pd.lambda_if);
        v1  = (pd.sigma_if_m * pd.sigma_if_m
               + beta2 * beta2 * pd.sigma_wl * pd.sigma_wl)
              / (pd.lambda_if * pd.lambda_if);
        // E4 over-determination screen: a wrong detector-rounded WL integer
        // (±1 cyc) sits 2.5–5σ off the E1+E4 float plane — the propagated σ
        // cannot see it (the mw_only lesson: propagation carries measurement
        // noise, not hypothesis risk), the plane residual can.
        if (pd.has_gf && std::isfinite(pd.dgf_m) && pd.sigma_gf_m > 0.0) {
            const double pred = lam1 * dn1 - lam2 * dn2;
            const double sres = std::sqrt(pd.sigma_gf_m * pd.sigma_gf_m
                                          + lam1 * lam1 * v1 + lam2 * lam2 * v2);
            if (sres > 0.0) {
                r.plane_resid_sigma = std::abs(pd.dgf_m - pred) / sres;
                if (r.plane_resid_sigma > kBridgePlaneResidMax) return r;
            }
        }
    } else if (pd.has_gf) {
        // E1+E4 route: [β1 −β2; λ1 −λ2]·ΔN = [δIF; dGF], full 2×2 covariance
        // propagation with the shared-noise off-diagonal ρ·σIF·σGF.
        if (!std::isfinite(pd.dif_m) || !std::isfinite(pd.sigma_if_m) ||
            !std::isfinite(pd.dgf_m) || !(pd.sigma_gf_m > 0.0)) return r;
        const double det = -beta1 * lam2 + beta2 * lam1;  // ≈ −0.046 m² (GPS)
        if (std::abs(det) < 1e-6) return r;
        // M⁻¹ = (1/det)·[−λ2, β2; −λ1, β1]
        const double a11 = -lam2 / det, a12 = beta2 / det;
        const double a21 = -lam1 / det, a22 = beta1 / det;
        dn1 = a11 * pd.dif_m + a12 * pd.dgf_m;
        dn2 = a21 * pd.dif_m + a22 * pd.dgf_m;
        const double sif = pd.sigma_if_m, sgf = pd.sigma_gf_m;
        const double c12 = rho_if_gf * sif * sgf;
        v1 = a11 * a11 * sif * sif + 2.0 * a11 * a12 * c12 + a12 * a12 * sgf * sgf;
        v2 = a21 * a21 * sif * sif + 2.0 * (a21 * a22) * c12 + a22 * a22 * sgf * sgf;
        cov12 = a11 * a21 * sif * sif + (a11 * a22 + a12 * a21) * c12
                + a12 * a22 * sgf * sgf;
        r.used_gf = true;
    } else {
        return r;   // no second equation yet — keep pending
    }
    if (!std::isfinite(dn1) || !std::isfinite(dn2)) return r;
    if (std::abs(dn1) > max_abs_cyc || std::abs(dn2) > max_abs_cyc) return r;
    if (!(v1 >= 0.0) || !(v2 >= 0.0)) return r;
    if (std::sqrt(v1) > max_sigma_cyc || std::sqrt(v2) > max_sigma_cyc) return r;

    r.ok = true;
    r.dn1_cyc = dn1;  r.dn2_cyc = dn2;
    r.shift1_m = lam1 * dn1;  r.shift2_m = lam2 * dn2;
    r.var1_m2 = lam1 * lam1 * v1;  r.var2_m2 = lam2 * lam2 * v2;

    // Stage 2: chain band 3 from the EWL integer.  dn_ewl_b3 was NORMALIZED at
    // fill time to ΔN3 − ΔN_shared, so the chain is sign-free here; the λ3
    // derivation folds the EWL wavelength against the shared band's own
    // (1/λ3 = 1/λ_shared ± 1/λ_EWL, + when band 3 is the higher frequency).
    if (pd.has_ewl && (pd.ewl_shared_band == 1 || pd.ewl_shared_band == 2)
        && pd.lambda_ewl > 0.0) {
        const double lam_sh = (pd.ewl_shared_band == 1) ? lam1 : lam2;
        const double dn_sh  = (pd.ewl_shared_band == 1) ? dn1 : dn2;
        const double v_sh   = (pd.ewl_shared_band == 1) ? v1 : v2;
        const double inv_l3 = 1.0 / lam_sh
                              + (pd.ewl_b3_hi ? 1.0 : -1.0) / pd.lambda_ewl;
        if (inv_l3 > 0.0) {
            const double lam3 = 1.0 / inv_l3;
            const double dn3 = dn_sh + pd.dn_ewl_b3;
            const double v3 = v_sh + pd.sigma_ewl * pd.sigma_ewl;
            (void)cov12;  // band-3 var uses the shared band's own var; the
                          // cross term to the OTHER band is not carried (the
                          // replace is diagonal by design).
            if (std::isfinite(dn3) && std::abs(dn3) <= max_abs_cyc
                && std::sqrt(v3) <= max_sigma_cyc) {
                r.has3 = true;
                r.dn3_cyc = dn3;
                r.shift3_m = lam3 * dn3;
                r.var3_m2 = lam3 * lam3 * v3;
                r.lam3_m = lam3;
            }
        }
    }
    return r;
}

bool networkSlipFillEwlSlot(RelevelPend& pd, const MwBridgeCand& cand,
                            double sigma_floor_eff_cyc) {
    if (pd.has_ewl) return false;
    const bool s1_shared = (cand.sig1 == pd.sig1 || cand.sig1 == pd.sig2);
    const bool s2_shared = (cand.sig2 == pd.sig1 || cand.sig2 == pd.sig2);
    if (s1_shared == s2_shared) return false;  // both shared (IS the IF pair) or disjoint
    const std::string& shared = s1_shared ? cand.sig1 : cand.sig2;
    const std::string& band3  = s1_shared ? cand.sig2 : cand.sig1;
    if (!pd.sig3.empty() && band3 != pd.sig3) return false;
    if (!(cand.lambda_wl > 0.0)) return false;
    // cand.dn_wl = ΔN(cand.sig1) − ΔN(cand.sig2) in the detector's
    // frequency-sorted order; normalize to ΔN(band3) − ΔN(shared).
    pd.has_ewl        = true;
    pd.dn_ewl_b3      = s1_shared ? -cand.dn_wl : cand.dn_wl;
    pd.ewl_b3_hi      = !s1_shared;  // band3 == cand.sig1 ⇒ the higher frequency
    pd.ewl_shared_band = (shared == pd.sig1) ? 1 : 2;
    pd.lambda_ewl     = cand.lambda_wl;
    pd.sigma_ewl      = std::max(cand.sigma_wl_cyc, sigma_floor_eff_cyc);
    pd.sig3           = band3;
    return true;
}

BridgeReplaceOutcome networkSlipBridgeV3Replace(
        const RelevelPend& pd, int sta, const std::string& sat,
        std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
        double rho_if_gf, double max_abs_cyc, double max_sigma_cyc,
        double corrob_cyc, BridgeV3Solve* solved) {
    const auto bs = networkSlipBridgeV3Solve(pd, rho_if_gf,
                                             max_abs_cyc, max_sigma_cyc);
    if (solved) *solved = bs;
    if (!bs.ok) return BridgeReplaceOutcome::KernelReject;
    auto it1 = carry.find({sta, sat, pd.sig1});
    auto it2 = carry.find({sta, sat, pd.sig2});
    if (it1 == carry.end() || it2 == carry.end())
        return BridgeReplaceOutcome::NoCarry;
    const bool do3 = bs.has3 && pd.has_pre3 && !pd.sig3.empty();
    auto it3 = do3 ? carry.find({sta, sat, pd.sig3}) : carry.end();
    // Per-band wavelengths for the cycle-domain corroboration.
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    const double lam1 = c / f1, lam2 = c / f2;
    const double L1 = pd.pre1 + bs.shift1_m;
    const double L2 = pd.pre2 + bs.shift2_m;
    if (std::abs(L1 - it1->second.value_abs) / lam1 > corrob_cyc ||
        std::abs(L2 - it2->second.value_abs) / lam2 > corrob_cyc)
        return BridgeReplaceOutcome::CorrobReject;
    double L3 = 0.0;
    const bool apply3 = do3 && it3 != carry.end() && bs.lam3_m > 0.0;
    if (apply3) {
        L3 = pd.pre3 + bs.shift3_m;
        // NO partial apply: a band-3 corroboration failure rejects the whole
        // bridge — a half-replaced LOS mixes pre-slip and free-re-level frames.
        if (std::abs(L3 - it3->second.value_abs) / bs.lam3_m > corrob_cyc)
            return BridgeReplaceOutcome::CorrobReject;
    }
    it1->second.value_abs = L1;  it1->second.var = pd.pre_var1 + bs.var1_m2;
    it2->second.value_abs = L2;  it2->second.var = pd.pre_var2 + bs.var2_m2;
    if (apply3) {
        it3->second.value_abs = L3;
        it3->second.var = pd.pre_var3 + bs.var3_m2;
    }
    return BridgeReplaceOutcome::Applied;
}

BridgeReplaceOutcome networkSlipBridgeReplace(
        const RelevelPend& pd, int sta, const std::string& sat,
        std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
        double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
        DeltaBridgeSolve* solved) {
    const auto bs = networkSlipDeltaBridgeSolve(pd.dn_wl, pd.sigma_wl,
                                                pd.dif_m, pd.sigma_if_m,
                                                pd.lambda_wl, pd.lambda_if,
                                                max_abs_cyc, max_sigma_cyc);
    if (solved) *solved = bs;
    if (!bs.ok) return BridgeReplaceOutcome::KernelReject;
    auto it1 = carry.find({sta, sat, pd.sig1});
    auto it2 = carry.find({sta, sat, pd.sig2});
    if (it1 == carry.end() || it2 == carry.end())
        return BridgeReplaceOutcome::NoCarry;
    // Per-band wavelengths for the cycle-domain corroboration (same derivation
    // as the kernel's).
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    const double lam1 = c / f1, lam2 = c / f2;
    const double L1 = pd.pre1 + bs.shift1_m;
    const double L2 = pd.pre2 + bs.shift2_m;
    // CORROBORATION (§8.1): the bridged level must agree with the interim v1
    // free re-level.  Correct bridges err <= 0.5 cyc against truth and the free
    // level sits within ~2 cyc, while the audit's wrong bridges erred
    // 6-113 cyc — agreement separates the populations.
    if (std::abs(L1 - it1->second.value_abs) / lam1 > corrob_cyc ||
        std::abs(L2 - it2->second.value_abs) / lam2 > corrob_cyc)
        return BridgeReplaceOutcome::CorrobReject;
    // REPLACE: the local-step mutation discipline — value + var only; the
    // coupling regression and last_seen belong to the current harvest and a
    // pure level re-base leaves both valid.
    it1->second.value_abs = L1;  it1->second.var = pd.pre_var1 + bs.var1_m2;
    it2->second.value_abs = L2;  it2->second.var = pd.pre_var2 + bs.var2_m2;
    return BridgeReplaceOutcome::Applied;
}

void networkSlipDrainMwRefloat(const PassABuffers& pa, const GnssConfig& config_,
                               std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               const std::vector<std::vector<PppObservation>>& modeled,
                               int i_lo, int n_sta_total,
                               bool carry_invariant, bool carry_warm,
                               std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                               const RawSlipEpoch& raw, const SlipQcParams& qc,
                               std::vector<RawSlipHistory>& raw_hist,
                               SlipQcCounters& qc_cnt,
                               const SyncedEpoch& se, NetworkRunTraces& traces,
                               const std::shared_ptr<spdlog::logger>& log,
                               std::set<std::string>* datum_pb_reref_sats,
                               std::set<std::string>* datum_amb_reref_sats,
                               std::set<std::string>* datum_mark_sats,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>* datum_local_carry,
                               RelevelEpochState* relevel,
                               std::map<std::tuple<int, std::string, std::string>,
                                        RelevelPend>* relevel_pend,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>* bridge_local_carry,
                               NetworkSlipOracle* oracle) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_mw_refloat = pa.sta_mw_refloat;
    // SIM-ONLY ORACLE: this epoch's continuous-GPS seconds, the frame the truth
    // table is indexed in (same expression as the wind-up drain's now_s).
    const double oracle_now_s =
        static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
    // Thin event forwarder — textual copy of the run-TU lambda, so the moved
    // emitEvent call sites are unchanged (same rows, same order).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // MW DETECTOR (mw_detect) → refloat: the iono-safe complement to IF-phase TD.
    // Two signals, neither of which uses the geometry-free ionosphere (so both stay
    // valid at the 30 s product rate, unlike the TurboEdit MW+GF integer REPAIR):
    //   (a) a CONFIDENT wide-lane integer jump collected in PASS A (sta_mw_refloat);
    //   (b) the receiver's own LLI loss-of-lock flag (bit 0) on a carried signal —
    //       the ground truth a receiver sets on a cycle slip / re-acquisition (this
    //       is what a real slip like G23@GAMB carries when MW/IF can't see it).
    // Either REFLOATS the affected ambiguity (marks it pending-reseed so PASS B
    // re-estimates it from this epoch's data) — never applying an integer, so a wrong
    // wide-lane integer can never bias the gauge.  Serial, station-ordered →
    // deterministic.  The refloat is meaningful only when the carry is warm; cold, the
    // detection is diagnostic (the free per-epoch estimate needs no reseed).  Pivot
    // (stations[0]) is never touched (its ambiguities are gauge-pinned).
    if (config_.networkCycleSlipMwDetect()) {
        // Card E: MATCHED (a detection landed on a live ambiguity) and REFLOATED
        // (the warm carry was actually reset) are separate facts.  Conflating
        // them is exactly what made every cold run report zero MW/LLI events
        // while still feeding the C3c funnel (limitation L3).
        struct SignalAction { bool matched{false}; bool refloated{false};
            // SIM-ONLY ORACLE (network.cycle_slip.oracle_relevel): the arc was
            // ANSWERED by a truth-fed level repair (or a truth-fed false-alarm
            // keep) instead of being freed.  `refloated` stays FALSE — nothing
            // was refloated — so every downstream gate that keys on it (the
            // bridge candidate push, the refloat counters) is correctly bypassed;
            // the event row's action is overridden from these instead.
            bool oracle_handled{false}; bool oracle_repaired{false};
            std::string oracle_detail; };
        // §7.6(c): `cause` is the lane observatory's freeing-cause bit
        // (RelevelEpochState::kCauseMw / kCauseLli) — measure-only tag, OR-merged
        // so a doubly-freed key keeps both bits.
        auto refloat_signal = [&](int i, const std::string& sat,
                                  const std::string& signal,
                                  std::uint8_t cause) -> SignalAction {
            SignalAction act;
            StateManager& sm = *stations[i].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                if (parts->signal == signal) {
                    act.matched = true;
                    // SIM-ONLY ORACLE slip response (include/slip/NetworkSlipOracle.hpp):
                    // a perfect repair REPLACES the free.  When it handles the arc,
                    // EVERY freeing action below is skipped — C3c funnel insert,
                    // pending-reseed mark, free_ambs latch, bridge-pend void —
                    // and only the event row survives, with an oracle action.
                    // Null (and unreachable) unless the sim-only knob is armed.
                    if (oracle) {
                        OracleOutcome oc = OracleOutcome::NotHandled;
                        std::string det;
                        // `cause` is this site's own tier bit (kCauseMw for the
                        // wide-lane drain, kCauseLli for the receiver's LLI) —
                        // it steers only the false-alarm KEEP attribution.
                        if (networkSlipOracleHandle(*oracle, i, sat, signal,
                                                    oracle_now_s, cause, det, oc)) {
                            act.oracle_handled  = true;
                            act.oracle_repaired = (oc == OracleOutcome::Repaired);
                            act.oracle_detail   = det;
                            continue;
                        }
                    }
                    // Card 4 C3c: feed the DD invariant carry from epoch 1,
                    // independent of the legacy carry's warm gate (the DD carry is
                    // live throughout; pending-reseed cannot serve it — PASS-B
                    // MeasurementModel consumes+clears it before the fold).
                    if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                    // Legacy phase-leveling refloat: only meaningful once warm.
                    if (carry_warm) { sm.markAmbiguityPendingReseed(amb_key); act.refloated = true;
                        // SPEC_slip_relevel_walk: the mark alone never delivered the
                        // free epoch (PASS-B consumes it pre-inject) — latch the set
                        // the inject site actually reads.
                        if (relevel) {
                            relevel->free_ambs.insert({i, amb_key});
                            relevel->free_cause[{i, amb_key}] |= cause;
                            ++qc_cnt.relevel_free_marked;
                        }
                        // Bridge v2: a fresh mark on this (station, sat) makes any
                        // outstanding pend's pre-slip capture STALE — void it (the
                        // T1 flag path re-creates its own pend after this).
                        if (relevel_pend)
                            for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
                                if (std::get<0>(pit->first) == i
                                    && std::get<1>(pit->first) == sat) {
                                    ++qc_cnt.bridge_pend_voided;
                                    pit = relevel_pend->erase(pit);
                                } else ++pit;
                    }
                }
            }
            return act;
        };
        // Card L: the LLI qualification is armed only when the toggle is on AND
        // this epoch actually has raw evidence.  Without evidence LLI stays
        // decisive — a gate that fires on missing data would be worse than the
        // chatter it is meant to fix.
        const bool lli_qual = qc.lli_corroborate && raw.computed;

        // §9.7.10 datum-station slip detect (network.cycle_slip.datum_station_detect):
        // armed iff the caller passed the queues (legacy pivot datum only).  A slip
        // detected at station 0 cannot be answered with refloat_signal — its
        // ambiguities are gauge-pinned, and the level every emitted phase bias of
        // that satellite references just moved.  The response is a NETWORK
        // re-reference, split across the two edges invariant #7 names:
        //   queue edge (here, at detection): sat → pb queue (this epoch's stash
        //   HARD-skips its PhaseBias carry) + amb queue + datum_slip event/count;
        //   apply edge (the block below, at the FIRST drain after that skip-stash
        //   consumed): all-station ambiguity reseed + local-carry erase + disc
        //   mark + datum_reref event/count — so the integers re-reference in the
        //   SAME solve the bias re-seeds.
        const bool datum_active = datum_pb_reref_sats && datum_amb_reref_sats
                                  && datum_mark_sats;
        auto datum_slip_detect = [&](const std::string& sat, const std::string& detail) {
            const bool fresh = datum_amb_reref_sats->insert(sat).second;
            datum_pb_reref_sats->insert(sat);
            if (!fresh) return;   // already queued and not yet applied — dedupe
            ++qc_cnt.datum_slip_detections;
            emitEvent("T0_mw_detect", "datum_slip", sat, stations[0].name,
                      std::nullopt, std::nullopt, "queue_reref", detail);
            log->warn("Network[datum-slip]: TOW {:.1f} slip detected at the DATUM station "
                      "{} on {} ({}) — queueing network re-reference (phase-bias carry "
                      "skip-stash this epoch; all-station ambiguity reseed in the re-seed "
                      "solve; per-sat disc mark at emit).",
                      se.time.tow, stations[0].name, sat, detail);
        };
        // Apply edge.  A sat still pending in the pb queue (its detection epoch was
        // WITHHELD, so the stash never consumed the skip) stays deferred — the two
        // edges cannot decouple across withholds.  Station 0 itself is never
        // reseeded: the pin at the new phase level IS the new reference.  Absent
        // stations are reseeded too — a pending-reseed mark persists until that
        // station's PASS B next runs, which is exactly its re-coupling edge.
        if (datum_active && !datum_amb_reref_sats->empty()) {
            for (auto it = datum_amb_reref_sats->begin();
                 it != datum_amb_reref_sats->end();) {
                const std::string sat = *it;
                if (datum_pb_reref_sats->count(sat)) { ++it; continue; }
                int n_marked = 0;
                for (int i = 1; i < n_sta_total; ++i) {
                    StateManager& sm = *stations[i].sm;
                    for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                        if (!parts || parts->satellite != sat) continue;
                        if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                        sm.markAmbiguityPendingReseed(amb_key);
                        if (datum_local_carry
                            && datum_local_carry->erase(
                                   std::make_tuple(i, parts->satellite, parts->signal)))
                            ++qc_cnt.datum_slip_carry_erases;   // retirement census (D)
                        ++n_marked;
                    }
                }
                datum_mark_sats->insert(sat);
                // §7.7 handover cross-guard: the network-wide reseed is a family
                // frame step on every signal of `sat` — the T4-side lane
                // measurement must not difference a pre-step capture against a
                // post-step re-level.  Measure-only tag.
                if (relevel) relevel->datum_reref_sats.insert(sat);
                ++qc_cnt.datum_slip_rerefs;
                emitEvent("T0_mw_detect", "datum_reref", sat, stations[0].name,
                          std::nullopt, std::nullopt, "refloat",
                          fmt::format("datum re-reference: {} amb reseed(s) network-wide; "
                                      "pb re-seeds this solve; disc mark queued", n_marked));
                log->info("Network[datum-slip]: TOW {:.1f} applied network re-reference for "
                          "{} — {} ambiguity(ies) reseeded across {} station(s); the shared "
                          "phase bias re-seeds this solve and the next emit carries a "
                          "disc mark.", se.time.tow, sat, n_marked, n_sta_total - 1);
                it = datum_amb_reref_sats->erase(it);
            }
        }

        int n_mw = 0, n_lli = 0, n_lli_supp = 0;
        std::string mw_detail;
        // --- Bridge v2 pend sweep (once per epoch, serial): expire stale pends
        // and REPLACE the ones whose MW integer arrived same-epoch at the T1
        // flag (their corroborating free re-level was harvested last epoch).
        const bool bridge_v2 = relevel_pend && bridge_local_carry
                            && carry_warm
                            && config_.networkCycleSlipRelevelDeltaBridge();
        const double v2_max_cyc  = config_.networkCycleSlipRelevelBridgeMaxCycles();
        const double v2_max_sig  = config_.networkCycleSlipRelevelBridgeMaxSigmaCyc();
        const double v2_corrob   = config_.networkCycleSlipRelevelBridgeCorrobCyc();
        const double v2_window_s = config_.networkCycleSlipRelevelBridgePendWindowS();
        const double v2_now_s    = se.time.week * 604800.0 + se.time.tow;
        auto v2_replace = [&](const std::tuple<int, std::string, std::string>& key,
                              const RelevelPend& pd) {
            DeltaBridgeSolve bs;
            const int rsta = std::get<0>(key);
            const std::string& rsat = std::get<1>(key);
            const auto outcome = networkSlipBridgeReplace(
                pd, rsta, rsat, *bridge_local_carry,
                v2_max_cyc, v2_max_sig, v2_corrob, &bs);
            switch (outcome) {
                case BridgeReplaceOutcome::Applied:
                    ++qc_cnt.bridge_pairs_applied;
                    qc_cnt.bridge_max_abs_dn_cyc =
                        std::max({qc_cnt.bridge_max_abs_dn_cyc,
                                  std::abs(bs.dn1_cyc), std::abs(bs.dn2_cyc)});
                    emitEvent("T1_if_slip", "bridge_replace", rsat,
                              stations[rsta].name, bs.dn1_cyc, bs.dn2_cyc, "bridge",
                              fmt::format("{};{};lag_s={:.0f};sd1={:.3f};sd2={:.3f}",
                                          pd.sig1, pd.sig2, v2_now_s - pd.tow_s,
                                          std::sqrt(bs.var1_m2), std::sqrt(bs.var2_m2)));
                    break;
                case BridgeReplaceOutcome::KernelReject: ++qc_cnt.bridge_fallback_reject;    break;
                case BridgeReplaceOutcome::NoCarry:      ++qc_cnt.bridge_fallback_no_carry;  break;
                case BridgeReplaceOutcome::CorrobReject: ++qc_cnt.bridge_replace_corrob_reject; break;
            }
            return outcome;
        };
        // Bridge v3 dispatch: when either rider is armed, every replace routes
        // through the v3 kernel (identical math to v2 on an E2-only pend, plus
        // the plane-residual screen when the GF row is present and the band-3
        // chain when the EWL slot is filled).
        const bool bridge_v3_gf  = bridge_v2 && config_.networkCycleSlipRelevelBridgeGf();
        const bool bridge_v3_ewl = bridge_v2 && config_.networkCycleSlipRelevelBridgeEwl();
        const bool bridge_v3     = bridge_v3_gf || bridge_v3_ewl;
        auto v3_replace = [&](const std::tuple<int, std::string, std::string>& key,
                              const RelevelPend& pd) {
            BridgeV3Solve bs;
            const int rsta = std::get<0>(key);
            const std::string& rsat = std::get<1>(key);
            const auto outcome = networkSlipBridgeV3Replace(
                pd, rsta, rsat, *bridge_local_carry, kBridgeGfIfRho,
                v2_max_cyc, v2_max_sig, v2_corrob, &bs);
            switch (outcome) {
                case BridgeReplaceOutcome::Applied: {
                    ++qc_cnt.bridge_pairs_applied;
                    if (bs.used_gf) ++qc_cnt.bridge_gf_applied;
                    const bool did3 = bs.has3 && pd.has_pre3;
                    if (did3) ++qc_cnt.bridge_band3_applied;
                    qc_cnt.bridge_max_abs_dn_cyc =
                        std::max({qc_cnt.bridge_max_abs_dn_cyc,
                                  std::abs(bs.dn1_cyc), std::abs(bs.dn2_cyc),
                                  did3 ? std::abs(bs.dn3_cyc) : 0.0});
                    emitEvent("T1_if_slip", "bridge_replace", rsat,
                              stations[rsta].name, bs.dn1_cyc, bs.dn2_cyc, "bridge",
                              fmt::format("{};{};lag_s={:.0f};sd1={:.3f};sd2={:.3f};"
                                          "route={}{}",
                                          pd.sig1, pd.sig2, v2_now_s - pd.tow_s,
                                          std::sqrt(bs.var1_m2), std::sqrt(bs.var2_m2),
                                          bs.used_gf ? "gf" : "mw",
                                          did3 ? fmt::format(";dn3={:.2f};sd3={:.3f}",
                                                             bs.dn3_cyc,
                                                             std::sqrt(bs.var3_m2))
                                               : std::string{}));
                    break;
                }
                case BridgeReplaceOutcome::KernelReject:
                    ++qc_cnt.bridge_fallback_reject;
                    if (bs.plane_resid_sigma > kBridgePlaneResidMax)
                        ++qc_cnt.bridge_plane_reject;
                    break;
                case BridgeReplaceOutcome::NoCarry:      ++qc_cnt.bridge_fallback_no_carry;  break;
                case BridgeReplaceOutcome::CorrobReject: ++qc_cnt.bridge_replace_corrob_reject; break;
            }
            return outcome;
        };
        auto do_replace = [&](const std::tuple<int, std::string, std::string>& key,
                              const RelevelPend& pd) {
            return bridge_v3 ? v3_replace(key, pd) : v2_replace(key, pd);
        };
        if (bridge_v2)
            for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();) {
                if (v2_now_s - pit->second.tow_s > v2_window_s) {
                    ++qc_cnt.bridge_pend_expired;
                    pit = relevel_pend->erase(pit);
                } else if (pit->second.has_dn
                           || (bridge_v3_gf && pit->second.has_gf)) {
                    do_replace(pit->first, pit->second);
                    pit = relevel_pend->erase(pit);   // terminal either way
                } else ++pit;
            }
        for (int i = (datum_active ? 0 : i_lo); i < n_sta_total; ++i) {
            if (!present[i]) continue;
            // (a) confident wide-lane integer slips detected in PASS A.  A satellite
            // can slip on several signal-pairs at once (multi-freq); refloat every
            // band but count / log / emit ONCE per (station, satellite).
            std::set<std::string> mw_done;
            for (const auto& cand : sta_mw_refloat[i]) {
                const auto& sat  = cand.sat;
                const auto& sig1 = cand.sig1;
                const auto& sig2 = cand.sig2;
                if (datum_active && i == 0) {
                    // Datum station: a confident WL integer jump takes the network
                    // re-reference path — never refloat_signal (pinned ambiguities).
                    if (mw_done.insert(sat).second)
                        datum_slip_detect(sat, "MW;" + sig1 + ";" + sig2);
                    continue;
                }
                // Bridge v2 intercept BEFORE the mark: the belated MW integer for
                // a slip T1 already flagged (and freed) 1-2 epochs ago.  Fill the
                // waiting pend and REPLACE immediately (the corroborating free
                // re-level was harvested at least one epoch ago).  On Applied the
                // normal mark/free path is SUPPRESSED — the slip's event already
                // fired at the flag epoch, and a second free would discard the
                // just-replaced level.  Every other outcome falls through to
                // today's path (the arc frees again — conservative).
                if (bridge_v2) {
                    const auto pkey = std::make_tuple(i, sat, sig1 + "_" + sig2);
                    const auto pit = relevel_pend->find(pkey);
                    if (pit != relevel_pend->end() && !pit->second.has_dn) {
                        pit->second.has_dn    = true;
                        pit->second.dn_wl     = cand.dn_wl;
                        pit->second.sigma_wl  = std::max(cand.frac,
                            config_.networkCycleSlipRelevelBridgeWlSigmaFloorCyc());
                        pit->second.lambda_wl = cand.lambda_wl;
                        const auto outcome = do_replace(pkey, pit->second);
                        relevel_pend->erase(pit);
                        if (outcome == BridgeReplaceOutcome::Applied) {
                            if (mw_done.insert(sat).second) ++qc_cnt.mw_detections;
                            continue;   // suppress the second free — level replaced
                        }
                    } else if (bridge_v3_ewl && pit == relevel_pend->end()) {
                        // Bridge v3 E3, belated: an EWL candidate whose pair
                        // shares exactly one band with a waiting (sta, sat)
                        // pend fills its band-3 slot.  No replace here — the
                        // sweep (or the belated IF-pair MW) closes the (1,2)
                        // plane; the EWL alone cannot.  The σ floor is mapped
                        // at the pend's FLAG-epoch elevation.
                        for (auto& [k3, pd3] : *relevel_pend) {
                            if (std::get<0>(k3) != i || std::get<1>(k3) != sat)
                                continue;
                            const double s3 = std::sin(
                                pd3.el_deg * 0.017453292519943295);
                            MwBridgeCand c3{i, sat, sig1, sig2, cand.dn_wl,
                                            std::max(cand.frac,
                                config_.networkCycleSlipRelevelBridgeWlSigmaFloorCyc()),
                                            cand.lambda_wl};
                            if (networkSlipFillEwlSlot(
                                    pd3, c3,
                                    config_.networkCycleSlipRelevelBridgeEwlSigmaFloorCyc()
                                        / std::max(s3, 0.15)))
                                ++qc_cnt.bridge_ewl_belated;
                            break;
                        }
                    }
                }
                const auto a = refloat_signal(i, sat, sig1,
                                              RelevelEpochState::kCauseMw);
                const auto b = refloat_signal(i, sat, sig2,
                                              RelevelEpochState::kCauseMw);
                // Bridge v2: the same-epoch candidate (MW and T1 firing together)
                // is deferred into the IF-TD driver, which stores the integer IN
                // the pend it creates this epoch; the replace then runs at the
                // NEXT epoch's sweep, once the corroborating free re-level exists.
                if (relevel && carry_warm && config_.networkCycleSlipRelevelDeltaBridge()
                    && (a.refloated || b.refloated))
                    relevel->mw_cands.push_back(MwBridgeCand{
                        i, sat, sig1, sig2, cand.dn_wl,
                        std::max(cand.frac,
                                 config_.networkCycleSlipRelevelBridgeWlSigmaFloorCyc()),
                        cand.lambda_wl});
                const bool matched   = a.matched   || b.matched;
                const bool refloated = a.refloated || b.refloated;
                // SIM-ONLY ORACLE: the detection is unchanged (same population,
                // same row), only the ACTION and the detail differ — so the arm
                // and its control are directly comparable in network_events.csv.
                const bool orc_h = a.oracle_handled || b.oracle_handled;
                const bool orc_r = a.oracle_repaired || b.oracle_repaired;
                // The event is one row per SATELLITE but the oracle decides per
                // BAND, so the detail must come from the band the ACTION names —
                // a "oracle_repair" row carrying the other band's "delta=0;n=0"
                // would make the trace disagree with itself.
                const std::string orc_detail =
                    a.oracle_repaired ? a.oracle_detail
                  : b.oracle_repaired ? b.oracle_detail
                  : a.oracle_handled  ? a.oracle_detail : b.oracle_detail;
                if (matched && mw_done.insert(sat).second) {
                    emitEvent("T0_mw_detect", "mw_wl_slip", sat, stations[i].name,
                              std::nullopt, std::nullopt,
                              orc_h ? (orc_r ? "oracle_repair" : "oracle_keep")
                                    : (refloated ? "refloat" : "diagnostic"),
                              orc_h ? (sig1 + ";" + sig2 + ";" + orc_detail)
                                    : (sig1 + ";" + sig2));
                    ++qc_cnt.mw_detections;
                    if (refloated) ++qc_cnt.mw_refloats;
                    if (++n_mw <= 12)
                        mw_detail += fmt::format("{}{}/{}", mw_detail.empty() ? "" : ", ",
                                                 stations[i].name, sat);
                }
            }
            // (b) receiver LLI loss-of-lock (bit 0) on any carried phase signal.
            // modeled[i] holds one entry per satellite; refloat every LLI'd band but
            // count / emit ONCE per satellite (an LLI on both L1 and L2 is one slip).
            const bool credible =
                !lli_qual || (i < static_cast<int>(raw.lli_credible.size())
                              && raw.lli_credible[static_cast<std::size_t>(i)] != 0);
            for (const auto& obs : modeled[i]) {
                // Card L qualification is a per-(station, satellite) decision, so
                // establish "does this satellite assert LLI at all" before acting.
                bool asserts = false;
                for (const auto& [code, lli] : obs.lli_flags)
                    if (!code.empty() && code[0] == 'L' && (lli & 1)) { asserts = true; break; }
                if (!asserts) continue;

                bool corr_mw = false, corr_gf = false, measurable = false, offpair = false;
                if (lli_qual && i < static_cast<int>(raw.verdict.size())) {
                    const auto& vm  = raw.verdict[static_cast<std::size_t>(i)];
                    const auto  vit = vm.find(obs.satellite);
                    if (vit != vm.end() && vit->second.measured) {
                        measurable = true;
                        corr_mw    = vit->second.mw_step;
                        corr_gf    = vit->second.gf_step && !corr_mw;
                    }
                    // Scope honesty: the shared observables are formed on the
                    // PRIMARY dual-frequency pair, so an LLI asserted only on a
                    // band OUTSIDE that pair (e.g. L5 on an L1/L2-primary arc)
                    // is not something MW/GF can witness.  Counted separately so
                    // "uncorroborated" never silently absorbs "unwitnessable" —
                    // a nonzero off-pair count is the trigger to widen the
                    // corroboration primitive to every tracked pair.
                    if (measurable) {
                        bool on_pair = false;
                        for (const auto& r : raw.rows[static_cast<std::size_t>(i)]) {
                            if (r.satellite != obs.satellite) continue;
                            for (const auto& [code, lli] : obs.lli_flags)
                                if (!code.empty() && code[0] == 'L' && (lli & 1)
                                    && (code == r.sig1 || code == r.sig2)) { on_pair = true; break; }
                            break;
                        }
                        offpair = !on_pair;
                    }
                }
                const bool corroborated = corr_mw || corr_gf;
                // ACT when the assertion is corroborated by the receiver's own
                // observables (decisive at ANY station — this is what keeps a real
                // multi-satellite re-lock intact, spec M3), or when the station's
                // LLI is still credible (spec M1 — do NOT require corroboration
                // everywhere; that would forfeit the G23@GAMB class of catches
                // where MW and IF genuinely cannot see the slip).
                const bool act = !lli_qual || corroborated || credible;

                if (lli_qual) {
                    RawSlipHistory* h = (static_cast<std::size_t>(i) < raw_hist.size())
                                            ? &raw_hist[static_cast<std::size_t>(i)] : nullptr;
                    ++qc_cnt.lli_asserted;
                    if (h) ++h->lli_asserted;
                    if (corr_mw)      { ++qc_cnt.lli_corr_mw; if (h) ++h->lli_corr_mw; }
                    else if (corr_gf) { ++qc_cnt.lli_corr_gf; if (h) ++h->lli_corr_gf; }
                    if (!measurable)  { ++qc_cnt.lli_unmeasurable; if (h) ++h->lli_unmeasurable; }
                    if (offpair)      { ++qc_cnt.lli_offpair;      if (h) ++h->lli_offpair; }
                    if (!act) {
                        ++qc_cnt.lli_suppressed;
                        if (h) ++h->lli_suppressed;
                        ++n_lli_supp;
                        // M4: never silent.  The suppressed assertion is a row of
                        // its own, carrying the station's credibility score so the
                        // top-offenders list is reconstructible from the trace.
                        emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                                  stations[i].name,
                                  h ? std::optional<double>(h->lli_frac_ewma) : std::nullopt,
                                  std::optional<double>(qc.chatter_frac_hi),
                                  "suppressed",
                                  !measurable ? "LLI;uncorroborated;unmeasurable"
                                              : (offpair ? "LLI;uncorroborated;off_primary_pair"
                                                         : "LLI;uncorroborated"));
                        continue;
                    }
                    if (h) ++h->lli_acted;
                }

                if (datum_active && i == 0) {
                    // Datum station: a qualified LLI assertion (the Card-L
                    // suppression above applies identically — a chattering pivot
                    // receiver must not re-reference the network repeatedly)
                    // takes the network re-reference path.
                    datum_slip_detect(obs.satellite,
                                      corr_mw ? "LLI;mw_step"
                                              : (corr_gf ? "LLI;gf_step" : "LLI"));
                    continue;
                }

                bool matched = false, refloated = false;
                bool orc_h = false, orc_r = false;
                std::string orc_detail;
                for (const auto& [code, lli] : obs.lli_flags) {
                    if (code.empty() || code[0] != 'L' || !(lli & 1)) continue;
                    const auto sa = refloat_signal(i, obs.satellite, code,
                                                   RelevelEpochState::kCauseLli);
                    if (sa.matched)   matched   = true;
                    if (sa.refloated) refloated = true;
                    // SIM-ONLY ORACLE: an LLI can be asserted on several bands of
                    // one satellite and each is handled INDEPENDENTLY (a slip on
                    // L1C only shifts L1C).  The event stays one-per-satellite,
                    // so it carries the detail of the band its ACTION names — the
                    // first REPAIRED band, else the first handled one.
                    if (sa.oracle_handled) {
                        orc_h = true;
                        if (sa.oracle_repaired && !orc_r) {
                            orc_r = true;
                            orc_detail = sa.oracle_detail;   // the repair wins
                        } else if (orc_detail.empty()) {
                            orc_detail = sa.oracle_detail;
                        }
                    }
                }
                if (matched) {
                    emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                              stations[i].name, std::nullopt, std::nullopt,
                              orc_h ? (orc_r ? "oracle_repair" : "oracle_keep")
                                    : (refloated ? "refloat" : "diagnostic"),
                              (corr_mw ? std::string("LLI;mw_step")
                                       : (corr_gf ? std::string("LLI;gf_step")
                                                  : std::string("LLI")))
                                  + (orc_h ? (";" + orc_detail) : std::string{}));
                    ++n_lli;
                    ++qc_cnt.lli_detections;
                    if (refloated) ++qc_cnt.lli_refloats;
                }
            }
        }
        if (n_mw > 0 || n_lli > 0 || n_lli_supp > 0)
            log->info("Network[MW detect]: TOW {:.1f} {} {} wide-lane slip(s){} + "
                      "{} LLI loss-of-lock(s){}", se.time.tow,
                      carry_warm ? "refloated" : "detected (carry cold; diagnostic)",
                      n_mw,
                      mw_detail.empty() ? std::string{} : (" [" + mw_detail + "]"), n_lli,
                      n_lli_supp > 0
                          ? fmt::format("; {} uncorroborated LLI assertion(s) SUPPRESSED "
                                        "at non-credible station(s)", n_lli_supp)
                          : std::string{});
    }
}

void networkSlipIfTdDriver(const PassABuffers& pa, const GnssConfig& config_,
                           std::vector<NetStation>& stations,
                           const std::vector<bool>& present,
                           int i_lo, int n_sta_total,
                           bool carry_invariant, bool carry_warm,
                           std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                           bool trace_flag_rates, FlagRate& fr_if_slip,
                           const SyncedEpoch& se, NetworkRunTraces& traces,
                           const std::shared_ptr<spdlog::logger>& log,
                           std::set<std::string>* refsat_mark_sats,
                           RelevelEpochState* relevel,
                           std::map<std::tuple<int, std::string, std::string>,
                                    LocalCarry>* bridge_local_carry,
                           SlipQcCounters* bridge_cnt,
                           std::map<std::tuple<int, std::string, std::string>,
                                    RelevelPend>* relevel_pend,
                           const RawSlipEpoch* raw_epoch,
                           const std::vector<RawSlipHistory>* raw_hist,
                           NetworkSlipOracle* oracle) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_if_resets   = pa.sta_if_resets;
    auto&       if_slip_trace   = traces.if_slip_trace;
    // Thin event forwarder — textual copy of the run-TU lambda (see above).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // IF-phase triple-difference slip detection + refloat (this increment).
    // Each non-pivot station's detector ran in DEFERRED mode above: it computed
    // the between-satellite, between-epoch ionosphere-free phase double
    // differences (ΔΔφ_IF) per (sat, signal-pair) and cached them, but did NOT
    // flag.  The driver decides, elevation-aware and per (station, signal-pair):
    //
    //  1. CENTER — pool ΔΔφ_IF across ALL present non-pivot stations BY SIGNAL
    //     PAIR, take the median `med` (raw m): the network common-mode (frame /
    //     datum wobble), which is elevation-independent, so it is removed in raw
    //     space.
    //  2. SCALE — normalize each residual to a zenith-equivalent, z = (dd−med)·
    //     sin(el) = (dd−med)/ifElevMap, so satellites at every elevation share one
    //     scale.  The pooled s_pool = 1.4826·median(|z|) is the warmup PRIOR; each
    //     (station, pair) also keeps an EWMA of its OWN z-scale (if_pair_scale).
    //     The effective scale shrinks from the pool toward the station's EWMA as
    //     that station-pair accumulates clean samples (empirical-Bayes, weight
    //     count/(count+κ)) — so a quiet reference station and a noisy modernized-
    //     signal station each get their OWN envelope, not one pooled threshold set
    //     by the majority (the fix for the L5Q/L2L over-firing).
    //  3. THRESHOLD sat i — max(k·s_eff·ifElevMap(el_i), floor_frac·λ_IF(1,1)):
    //     the empirical term re-inflated to sat i's elevation, the physical
    //     (1,1)-integer floor kept in raw m so a real slip is caught at ANY
    //     elevation.  Flag if |dd_i − med| exceeds it.
    //  4. A flagged arc has its detector gradient reset (fresh arc next epoch —
    //     arc hygiene, every epoch) and, when the carry is active, BOTH band
    //     ambiguities marked pending-reseed so PASS B refloats them.  Clean
    //     survivors then update the station-pair EWMA (slips excluded, so the
    //     scale never absorbs the event it must catch).
    //
    // The detector's own reference-satellite common-mode slip keys (which the
    // per-pair pool cannot see) are folded in too.  Thresholds are deliberately
    // loose; the sharp pass is a future post-estimation phase validation, not yet
    // built.  Serial, station-ordered → deterministic.  Pivot has no detector.
    if (config_.networkCycleSlipIfPhaseTd()) {
        const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
        const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
        const int    min_pool     = config_.networkCycleSlipIfPhaseTdMinPool();
        const double ewma_alpha   = config_.networkCycleSlipIfPhaseTdEwmaAlpha();
        const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

        // SIM-ONLY ORACLE outcome for one flagged IF pair — see refloat_signal's
        // SignalAction.  `handled` is ANY band; the pend gate below uses it,
        // because a bridge replace that re-bases one band while the other was
        // truth-repaired would overwrite the repair with a pre-slip level.
        struct PairAction { bool handled{false}; bool repaired{false};
                            bool freed{false}; std::string detail; };
        const double oracle_now_s =
            static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
        // Refloat both band ambiguities of (sat, sig1, sig2) at station i (only
        // when the carry is active; cold, the detection is diagnostic only).
        auto refloat_pair = [&](int i, const std::string& sat,
                                const std::string& sig1,
                                const std::string& sig2) -> PairAction {
            PairAction pact;
            StateManager& sm = *stations[i].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                if (parts->signal == sig1 || parts->signal == sig2) {
                    // SIM-ONLY ORACLE: each BAND is handled independently (the
                    // IF combination cannot localize the slip, but truth can) —
                    // handled ⇒ skip every freeing action for this band.
                    if (oracle) {
                        OracleOutcome oc = OracleOutcome::NotHandled;
                        std::string det;
                        if (networkSlipOracleHandle(*oracle, i, sat, parts->signal,
                                                    oracle_now_s,
                                                    RelevelEpochState::kCauseIfTd,
                                                    det, oc)) {
                            pact.handled = true;
                            // The event names ONE action for the pair, so its
                            // detail must be the band that action describes: the
                            // first REPAIRED band, else the first handled one.
                            if (oc == OracleOutcome::Repaired && !pact.repaired) {
                                pact.repaired = true;
                                pact.detail   = det;
                            } else if (pact.detail.empty()) {
                                pact.detail = det;
                            }
                            continue;
                        }
                    }
                    // Card 4 C3c: feed the DD carry from epoch 1 (see refloat_signal).
                    if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                    if (carry_warm) {
                        pact.freed = true;
                        sm.markAmbiguityPendingReseed(amb_key);
                        // SPEC_slip_relevel_walk: latch the free epoch the consumed
                        // mark never delivered (see refloat_signal).
                        if (relevel) {
                            relevel->free_ambs.insert({i, amb_key});
                            relevel->free_cause[{i, amb_key}] |=
                                RelevelEpochState::kCauseIfTd;
                            if (bridge_cnt) ++bridge_cnt->relevel_free_marked;
                        }
                        // Bridge v2: a fresh mark voids any outstanding pend for
                        // this (station, sat) — its pre-slip capture is stale.
                        // The T1 flag path re-creates its own pend AFTER this.
                        if (relevel_pend)
                            for (auto pit = relevel_pend->begin();
                                 pit != relevel_pend->end();)
                                if (std::get<0>(pit->first) == i
                                    && std::get<1>(pit->first) == sat) {
                                    if (bridge_cnt) ++bridge_cnt->bridge_pend_voided;
                                    pit = relevel_pend->erase(pit);
                                } else ++pit;
                    }
                }
            }
            return pact;
        };

        // Pool this epoch's ΔΔφ_IF by signal pair across present non-pivot stations.
        // Carry elevation (for the stochastic normalization) and the per-sat (1,1)
        // IF wavelength (for the physical floor — exact even for GLONASS FDMA).
        // is_ref marks the constellation REFERENCE satellite, whose dd is identically
        // 0 by construction.  Carried per sample so the robust SCALE below can drop it
        // (§6.14.5 defect 2); it stays in `samples` for the centering median and for
        // thresholding, so the flag population is unchanged.
        struct PairSample { int sta; std::string sat, sig1, sig2; double dd, el, lambda;
                            bool is_ref; };
        std::map<std::string, std::vector<PairSample>> by_pair;
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i] || !stations[i].if_slip) continue;
            for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                if (jp.sig1.empty() || jp.sig2.empty()) continue;
                const std::string pair = jp.sig1 + "_" + jp.sig2;
                by_pair[pair].push_back({i, sat, jp.sig1, jp.sig2,
                                         jp.delta_grad_m, jp.elevation_deg, jp.lambda_if_m,
                                         jp.is_reference});
            }
        }

        int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;
        std::string if_detail;

        // --- SPEC_slip_relevel_walk §8.2: bridge v2, PEND side ---------------
        // The flag epoch only CAPTURES: the pre-slip carry (this PASS-A map
        // still holds the previous harvest) and the measured δIF with its
        // empirical σ — only this scope has (dd − med) and s_eff·emap.  The
        // arc then takes the v1 free epoch.  The REPLACE (with the §8.1
        // corroboration against that free level) runs in the MW drain when the
        // confident WL integer arrives — measured 1–2 epochs later; a
        // same-epoch MW candidate is stored IN the pend (cand_idx below) and
        // replaced at the next epoch's sweep.
        const bool bridge_on = relevel && bridge_local_carry && bridge_cnt
                            && relevel_pend && carry_warm
                            && config_.networkCycleSlipRelevelDeltaBridge();
        std::map<std::tuple<int, std::string, std::string>, MwBridgeCand*> cand_idx;
        if (bridge_on)
            for (auto& c : relevel->mw_cands)
                cand_idx[{c.sta, c.sat, c.sig1 + "_" + c.sig2}] = &c;
        const double br_if_floor  = config_.networkCycleSlipRelevelBridgeIfSigmaFloorM();
        const double br_wl_floor  = config_.networkCycleSlipRelevelBridgeWlSigmaFloorCyc();
        const double br_now_s     = se.time.week * 604800.0 + se.time.tow;
        // Bridge v3 riders (loader guarantees relevel_delta_bridge when set).
        const bool   br_gf_on     = bridge_on
                                    && config_.networkCycleSlipRelevelBridgeGf()
                                    && raw_epoch && raw_hist;
        const bool   br_ewl_on    = bridge_on
                                    && config_.networkCycleSlipRelevelBridgeEwl();
        const double br_gf_floor  = config_.networkCycleSlipRelevelBridgeGfSigmaFloorM();
        const double br_ewl_floor = config_.networkCycleSlipRelevelBridgeEwlSigmaFloorCyc();
        // Elevation map for the EWL σ floor: the single-epoch EWL-MW noise is
        // NL-code-driven, ~1/sin(el); clamp low so a horizon arc cannot explode.
        auto ewlFloorEff = [&](double el_deg) {
            const double s = std::sin(el_deg * 0.017453292519943295);
            return br_ewl_floor / std::max(s, 0.15);
        };
        for (auto& [pair, samples] : by_pair) {
            // (1) Network common-mode center: median of the raw DDs (m).
            std::vector<double> raw;
            raw.reserve(samples.size());
            for (const auto& s : samples) raw.push_back(s.dd);
            std::nth_element(raw.begin(), raw.begin() + raw.size() / 2, raw.end());
            const double med = raw[raw.size() / 2];

            // (2) Pooled elevation-normalized robust scale (zenith-equiv m):
            //     s_pool = 1.4826·median(|z|), z = (dd−med)/ifElevMap(el).
            //     The constellation REFERENCE satellite is EXCLUDED here: its dd is
            //     identically 0 by construction (grad(ref) = OMC(ref) − OMC(ref) = 0),
            //     so it contributes an exact structural zero that sits at the centre of
            //     the distribution and DEFLATES the MAD.  Measured on
            //     runs/nonint_losblock24h_hv (24 h, 60 sta): those zeros are 12.5 % of
            //     all samples and the settled s_epoch p50 reads 0.01190 m with them vs
            //     0.01416 m without — a 16.0 % deflation.  The reference carries no
            //     information about the noise this threshold is meant to track, and the
            //     pool arm can never flag it anyway (|dd − med| ≈ 0) — the separate
            //     per-station reference gate (tier T1r) is what screens it.
            //     §6.14.5 defect 2.  Fallback: if excluding leaves nothing (a pair whose
            //     only sample IS the reference), keep the full set rather than emit a
            //     zero scale — s_eff would then collapse onto the physical floor.
            std::vector<double> zabs;
            zabs.reserve(samples.size());
            for (const auto& s : samples)
                if (!s.is_ref) zabs.push_back(std::abs((s.dd - med) / ifElevMap(s.el)));
            if (zabs.empty())
                for (const auto& s : samples)
                    zabs.push_back(std::abs((s.dd - med) / ifElevMap(s.el)));
            std::nth_element(zabs.begin(), zabs.begin() + zabs.size() / 2, zabs.end());
            const double s_pool = 1.4826 * zabs[zabs.size() / 2];
            if (static_cast<int>(samples.size()) < min_pool) ++n_sparse;

            // Group the pair's samples by station (ordered → deterministic) so each
            // station-pair thresholds with its OWN shrunk scale, then updates its
            // OWN EWMA once this epoch's flags are known.
            std::map<int, std::vector<const PairSample*>> by_sta;
            for (const auto& s : samples) by_sta[s.sta].push_back(&s);

            for (auto& [sta, plist] : by_sta) {
                auto& sc = stations[sta].if_pair_scale[pair];

                // Empirical-Bayes shrinkage: pool prior (cold) → station EWMA (warm).
                const double w = static_cast<double>(sc.count) /
                                 (static_cast<double>(sc.count) + shrink_kappa);
                double s_eff = (sc.count > 0) ? (w * sc.ewma_scale + (1.0 - w) * s_pool)
                                              : s_pool;
                if (!(s_eff > 0.0)) s_eff = s_pool;  // degenerate all-equal pool
                if (sc.count >= static_cast<long>(shrink_kappa)) ++n_warm_sp;

                std::vector<double> clean_zabs;
                for (const PairSample* sp : plist) {
                    const double emap    = ifElevMap(sp->el);
                    const double floor_m = floor_frac * sp->lambda;
                    const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                    const double abs_dev = std::abs(sp->dd - med);
                    const bool   flagged = (abs_dev > thr);
                    if (trace_flag_rates) { ++fr_if_slip.opp; if (flagged) ++fr_if_slip.flag; }  // B8/T1
                    if (flagged) {
                        stations[sp->sta].if_slip->resetSatellite(sp->sat);  // restart arc
                        const auto pact =
                            refloat_pair(sp->sta, sp->sat, sp->sig1, sp->sig2);
                        emitEvent("T1_if_slip", "if_slip", sp->sat,
                                  stations[sp->sta].name, abs_dev, thr,
                                  pact.handled
                                      ? (pact.repaired ? "oracle_repair" : "oracle_keep")
                                      : (carry_warm ? "refloat" : "diagnostic"),
                                  pact.handled
                                      ? (sp->sig1 + ";" + sp->sig2 + ";" + pact.detail)
                                      : (sp->sig1 + ";" + sp->sig2));
                        // Bridge v2, flagged path: CREATE (or refresh — the
                        // refloat_pair above just voided any stale one) the pend
                        // with the pre-slip capture + this δIF.  A station whose
                        // IF-TD refsat rebased this epoch is frame-invalid — no
                        // pend, the arc keeps the plain free epoch.
                        // SIM-ONLY ORACLE: with the level truth-repaired there is
                        // nothing to bridge, and a later replace would overwrite
                        // the repair with `pre_slip + shift` — no pend.
                        if (bridge_on && !pact.handled) {
                            if (!sta_if_resets[sp->sta].empty()) {
                                ++bridge_cnt->bridge_fallback_refsat;
                            } else {
                                auto it1 = bridge_local_carry->find(
                                    {sp->sta, sp->sat, sp->sig1});
                                auto it2 = bridge_local_carry->find(
                                    {sp->sta, sp->sat, sp->sig2});
                                if (it1 == bridge_local_carry->end()
                                    || it2 == bridge_local_carry->end()) {
                                    ++bridge_cnt->bridge_fallback_no_carry;
                                } else {
                                    RelevelPend pd;
                                    pd.pre1 = it1->second.value_abs;
                                    pd.pre_var1 = it1->second.var;
                                    pd.pre2 = it2->second.value_abs;
                                    pd.pre_var2 = it2->second.var;
                                    pd.dif_m = sp->dd - med;
                                    pd.sigma_if_m = std::max(s_eff * emap, br_if_floor);
                                    pd.lambda_if = sp->lambda;
                                    pd.sig1 = sp->sig1;  pd.sig2 = sp->sig2;
                                    pd.tow_s = br_now_s;
                                    pd.el_deg = sp->el;
                                    const auto ci = cand_idx.find(
                                        std::tuple<int, std::string, std::string>{
                                            sp->sta, sp->sat, pair});
                                    if (ci != cand_idx.end() && !ci->second->consumed) {
                                        pd.has_dn    = true;
                                        pd.dn_wl     = ci->second->dn_wl;
                                        pd.sigma_wl  = std::max(ci->second->sigma_wl_cyc,
                                                                br_wl_floor);
                                        pd.lambda_wl = ci->second->lambda_wl;
                                        ci->second->consumed = true;
                                        ++bridge_cnt->bridge_joint;   // same-epoch pairings
                                    }
                                    // Bridge v3 E4: the same-pair GF-phase TD
                                    // (geometry/clock-free) with the station's
                                    // OWN empirical step scale — closes the
                                    // (1,2) plane at the first sweep without
                                    // waiting for the MW integer.  The raw
                                    // primary pair must BE this IF pair (true
                                    // for GPS and GAL on the triple posture);
                                    // a mismatch or a gap re-birth (no adjacent
                                    // predecessor) keeps the pend E2-only.
                                    if (br_gf_on
                                        && sp->sta < static_cast<int>(
                                               raw_epoch->verdict.size())) {
                                        const auto& vm =
                                            raw_epoch->verdict[sp->sta];
                                        const auto vit = vm.find(sp->sat);
                                        const RawSlipObs* row = nullptr;
                                        if (vit != vm.end() && vit->second.measured)
                                            for (const auto& ro :
                                                 raw_epoch->rows[sp->sta])
                                                if (ro.satellite == sp->sat) {
                                                    row = &ro; break;
                                                }
                                        if (row && row->dual
                                            && row->sig1 == sp->sig1
                                            && row->sig2 == sp->sig2) {
                                            pd.has_gf = true;
                                            pd.dgf_m = vit->second.d_gf;
                                            pd.sigma_gf_m = std::max(
                                                (*raw_hist)[sp->sta].gf_step_scale,
                                                br_gf_floor);
                                            if (!(pd.lambda_wl > 0.0))
                                                pd.lambda_wl = row->lambda_wl_m;
                                            ++bridge_cnt->bridge_gf_captured;
                                        }
                                    }
                                    // Bridge v3 E3: record the third band and
                                    // capture its PRE-SLIP carry NOW — a belated
                                    // capture would read the interim free
                                    // re-level, not the pre-slip level.
                                    if (br_ewl_on) {
                                        for (const auto& [k3, i3] :
                                             stations[sp->sta].sm->getAmbiguities()) {
                                            const auto p3 =
                                                StateKeys::parseAmbiguityKey(k3);
                                            if (!p3 || p3->satellite != sp->sat)
                                                continue;
                                            if (p3->signal == sp->sig1
                                                || p3->signal == sp->sig2)
                                                continue;
                                            pd.sig3 = p3->signal;
                                            const auto it3 =
                                                bridge_local_carry->find(
                                                    {sp->sta, sp->sat, p3->signal});
                                            if (it3 != bridge_local_carry->end()) {
                                                pd.has_pre3 = true;
                                                pd.pre3 = it3->second.value_abs;
                                                pd.pre_var3 = it3->second.var;
                                            }
                                            break;
                                        }
                                        // Same-epoch EWL integer (the common
                                        // case — the unlocked MW path publishes
                                        // the instantly-roundable EWL at the
                                        // flag epoch, and the MW drain ran
                                        // before this driver).
                                        for (auto& c3 : relevel->mw_cands)
                                            if (!c3.consumed && c3.sta == sp->sta
                                                && c3.sat == sp->sat
                                                && networkSlipFillEwlSlot(
                                                       pd, c3, ewlFloorEff(sp->el))) {
                                                c3.consumed = true;
                                                ++bridge_cnt->bridge_ewl_joint;
                                                break;
                                            }
                                    }
                                    (*relevel_pend)[std::make_tuple(
                                        sp->sta, sp->sat, pair)] = pd;
                                    ++bridge_cnt->bridge_pends_created;
                                }
                            }
                        }
                        ++n_if_arcs;
                        if (n_if_arcs <= 12)
                            if_detail += fmt::format("{}{}/{}_{}_{}", if_detail.empty() ? "" : ", ",
                                                     stations[sp->sta].name, sp->sat, sp->sig1, sp->sig2);
                    } else if (!sp->is_ref) {
                        // §6.14.5 defect 2, WARM half: the reference's structural
                        // zero never flags (|dd − med| ≈ 0 by construction), so it
                        // always lands here and deflates s_epoch — which is the
                        // quantity the 16.0 % measurement was made on (p50 0.01190 m
                        // shipped vs 0.01416 m excluded).  s_eff = w·ewma + (1−w)·s_pool,
                        // so BOTH halves must drop it or the scale stays biased once
                        // the station-pair goes warm.  Excluding it also stops sc.count
                        // (the shrinkage weight) from crediting a non-observation.
                        clean_zabs.push_back(abs_dev / emap);
                        // Delta bridge, MW-only path: REMOVED after the truth
                        // audit (SPEC_slip_relevel_walk §8.1) — an MW candidate
                        // whose IF sample did NOT flag is dominated by the MW
                        // FALSE-ALARM population on a healthy arc (44 wrong + 16
                        // no-truth of 73 applied bridges, deltas to ±199 cyc at
                        // cm-tight propagated σ: the propagation carries the
                        // measurement noise, not the hypothesis risk).  The
                        // candidate stays unconsumed and is counted unpaired;
                        // the bands stay in the free set — the safe response.
                    }
                    // Deep-investigation row: the full decision context for this
                    // (station, sat, pair) sample.  sc.ewma_scale / sc.count are the
                    // decision-time (pre-update) values used to form s_eff.
                    if (if_slip_trace.is_open())
                        if_slip_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << stations[sp->sta].name << ',' << sp->sat << ','
                            << sp->sig1 << ',' << sp->sig2 << ','
                            << fmt::format("{:.2f}", sp->el) << ','
                            << fmt::format("{:.6f}", sp->dd) << ',' << fmt::format("{:.6f}", med) << ','
                            << fmt::format("{:.6f}", s_pool) << ',' << fmt::format("{:.6f}", sc.ewma_scale) << ','
                            << sc.count << ',' << fmt::format("{:.3f}", w) << ','
                            << fmt::format("{:.6f}", s_eff) << ',' << fmt::format("{:.3f}", emap) << ','
                            << fmt::format("{:.6f}", thr) << ',' << fmt::format("{:.6f}", floor_m) << ','
                            << fmt::format("{:.6f}", abs_dev) << ',' << (flagged ? 1 : 0) << ','
                            << (carry_warm ? 1 : 0) << '\n';
                }

                // (4) Fold clean survivors into the station-pair EWMA (slips excluded).
                if (!clean_zabs.empty()) {
                    std::nth_element(clean_zabs.begin(),
                                     clean_zabs.begin() + clean_zabs.size() / 2, clean_zabs.end());
                    const double s_epoch = 1.4826 * clean_zabs[clean_zabs.size() / 2];
                    sc.ewma_scale = (sc.count == 0)
                        ? s_epoch
                        : (1.0 - ewma_alpha) * sc.ewma_scale + ewma_alpha * s_epoch;
                    sc.count += static_cast<long>(clean_zabs.size());
                }
            }
        }

        // Delta bridge: candidates whose (station,sat,pair) never met an IF-TD
        // sample this epoch — their bands stay in the free set (safe fallback).
        if (bridge_on)
            for (const auto& c : relevel->mw_cands)
                if (!c.consumed) ++bridge_cnt->bridge_fallback_unpaired;

        // Reference-satellite common-mode slips (per station; pool can't see them).
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& rkey : sta_if_resets[i]) {
                std::string sat, sig1, sig2;
                if (!parseIfPairResetKey(rkey, sat, sig1, sig2)) continue;
                const auto pact = refloat_pair(i, sat, sig1, sig2);
                emitEvent("T1_if_slip", "refsat_reset", sat, stations[i].name,
                          std::nullopt, std::nullopt,
                          pact.handled
                              ? (pact.repaired ? "oracle_repair" : "oracle_keep")
                              : (carry_warm ? "refloat" : "diagnostic"),
                          pact.handled ? (sig1 + ";" + sig2 + ";" + pact.detail)
                                       : (sig1 + ";" + sig2));
                // Edge-1 piece 2 (network.mark_refsat_reset_disc; nullptr ⇒ OFF ⇒
                // byte-identical).  Queue the satellite for a per-sat discontinuity
                // mark at its next actual emit.  Gated on carry_warm for the same
                // reason the event's action field is: while the carry is cold the
                // detection is DIAGNOSTIC — no refloat happened, so there is no
                // datum re-reference for a consumer to react to, and marking one
                // would be a fabricated event.
                // SIM-ONLY ORACLE: an arc answered by a truth repair was NOT
                // re-referenced, so a disc mark here would be a fabricated event
                // (the same argument the carry_warm gate makes).  A pair where
                // one band was still freed keeps its mark.
                if (refsat_mark_sats && carry_warm && (pact.freed || !pact.handled))
                    refsat_mark_sats->insert(sat);
                ++n_if_arcs;
                if (n_if_arcs <= 12)
                    if_detail += fmt::format("{}{}/{} (refsat)", if_detail.empty() ? "" : ", ",
                                             stations[i].name, rkey);
            }
        }

        if (n_if_arcs > 0)
            log->info("Network[IF-phase slip]: TOW {:.1f} flagged {} arc(s){} across {} pair(s) "
                      "({} station-pair(s) warm){}{}",
                      se.time.tow, n_if_arcs,
                      carry_warm ? " for refloat" : " (carry cold; diagnostic)",
                      by_pair.size(), n_warm_sp,
                      n_sparse > 0 ? fmt::format(", {} pair(s) under min_pool({})", n_sparse, min_pool)
                                   : std::string{},
                      if_detail.empty() ? std::string{} : (" [" + if_detail + "]"));
    }
}

void networkSlipDrainWindupRestart(const GnssConfig& config_,
                                   std::vector<NetStation>& stations,
                                   const std::vector<bool>& present,
                                   int i_lo, int n_sta_total,
                                   bool carry_invariant, bool carry_warm,
                                   std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                   SlipQcCounters& qc_cnt,
                                   const SyncedEpoch& se, NetworkRunTraces& traces,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   RelevelEpochState* relevel,
                                   std::map<std::tuple<int, std::string, std::string>,
                                            RelevelPend>* relevel_pend,
                                   NetworkSlipOracle* oracle) {
    if (!config_.networkCycleSlipWindupRestartRefloat()) return;
    const double now_s = static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
    const double carry_gap_s = config_.networkPhaseLevelingMaxGapS();
    int n_resp = 0;
    std::string detail;
    for (int i = i_lo; i < n_sta_total && i < static_cast<int>(stations.size()); ++i) {
        // Absent station: its model was not evaluated this epoch, so its ledger
        // holds no fresh entry — leave it for the return epoch, where the
        // continued gap fires a FRESH restart that overwrites the stale one.
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        if (!stations[i].model || !stations[i].sm) continue;
        const auto& ledger = stations[i].model->wuRestarts();
        if (ledger.empty()) continue;
        for (const auto& [sat, rec] : ledger) {
            ++qc_cnt.wu_restart_events;
            if (rec.epoch_s != now_s) {
                // A restart from an EARLIER epoch (ledger survived an epoch
                // nobody drained — e.g. the knob path raced a withheld epoch).
                // The step it describes was already absorbed by whatever
                // happened since; responding NOW would refloat a healthy arc.
                ++qc_cnt.wu_restart_stale;
                traces.emitEvent(se.time, "T0_windup", "wu_restart", sat,
                                 stations[static_cast<std::size_t>(i)].name,
                                 rec.gap_s, carry_gap_s, "stale_refused", "");
                continue;
            }
            if (rec.gap_s > carry_gap_s) {
                // The gap outlived the carry horizon too: the leveling carry
                // was pruned with it, so there is nothing stale to refloat.
                ++qc_cnt.wu_restart_beyond_carry;
                traces.emitEvent(se.time, "T0_windup", "wu_restart", sat,
                                 stations[static_cast<std::size_t>(i)].name,
                                 rec.gap_s, carry_gap_s, "beyond_carry", "");
                continue;
            }
            StateManager& sm = *stations[static_cast<std::size_t>(i)].sm;
            int matched = 0;
            // SIM-ONLY ORACLE: this is the WHOLE-SATELLITE site — a restart steps
            // every signal of the satellite — so each SIGNAL is offered to the
            // oracle independently (truth localizes what the band-common model
            // step cannot).  A signal with no truth jump in the window follows the
            // false-alarm policy; the rest fall through to today's refloat.
            bool orc_h = false, orc_r = false, orc_freed = false;
            std::string orc_detail;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                ++matched;
                if (oracle) {
                    OracleOutcome oc = OracleOutcome::NotHandled;
                    std::string det;
                    if (networkSlipOracleHandle(*oracle, i, sat, parts->signal,
                                                now_s, RelevelEpochState::kCauseWindup,
                                                det, oc)) {
                        orc_h = true;
                        // One row per (station, satellite): the detail is the
                        // band the ACTION names — first repaired, else first
                        // handled (see the MW site).
                        if (oc == OracleOutcome::Repaired && !orc_r) {
                            orc_r = true;
                            orc_detail = det;
                        } else if (orc_detail.empty()) {
                            orc_detail = det;
                        }
                        continue;
                    }
                }
                // Card 4 C3c: the DD invariant carry owns its own funnel and is
                // live from epoch 1 (see refloat_signal).
                if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                if (carry_warm) {
                    orc_freed = true;
                    sm.markAmbiguityPendingReseed(amb_key);
                    if (relevel) {
                        relevel->free_ambs.insert({i, amb_key});
                        relevel->free_cause[{i, amb_key}] |=
                            RelevelEpochState::kCauseWindup;
                        ++qc_cnt.relevel_free_marked;
                    }
                }
            }
            // A restart is a MODEL step, not a slip: any outstanding bridge
            // pend's pre-slip capture predates the step and its replace would
            // re-inject a pre-restart-frame value — void it (the same rule the
            // raw-slip drains apply on a fresh mark).
            // SIM-ONLY ORACLE: with every signal truth-repaired nothing was
            // marked, so there is no stale capture to void (voiding one would
            // discard a pend the arc still needs).
            if (carry_warm && relevel_pend && (orc_freed || !orc_h))
                for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
                    if (std::get<0>(pit->first) == i && std::get<1>(pit->first) == sat) {
                        ++qc_cnt.bridge_pend_voided;
                        pit = relevel_pend->erase(pit);
                    } else ++pit;
            if (carry_warm && matched > 0) {
                ++qc_cnt.wu_restart_refloated;
                qc_cnt.wu_restart_arcs += matched;
                if (++n_resp <= 12)
                    detail += fmt::format("{}{}/{}", detail.empty() ? "" : ", ",
                                          stations[static_cast<std::size_t>(i)].name, sat);
            } else if (!carry_warm) {
                ++qc_cnt.wu_restart_cold;
            } else {
                // Warm but no live ambiguity matched the satellite — the arc
                // is not in this epoch's estimation set; count as refloated=0
                // via the event row's action so closure still holds.
                ++qc_cnt.wu_restart_refloated;
            }
            traces.emitEvent(se.time, "T0_windup", "wu_restart", sat,
                             stations[static_cast<std::size_t>(i)].name,
                             rec.gap_s, carry_gap_s,
                             orc_h ? (orc_r ? "oracle_repair" : "oracle_keep")
                                   : (carry_warm ? (matched > 0 ? "refloat" : "no_arc")
                                                 : "diagnostic"),
                             orc_h ? fmt::format("accumulator restart; {} arc(s); {}",
                                                 matched, orc_detail)
                                   : fmt::format("accumulator restart; {} arc(s) marked",
                                                 matched));
        }
        stations[static_cast<std::size_t>(i)].model->clearWuRestarts();
    }
    if (n_resp > 0)
        log->info("Network[windup-restart]: TOW {:.1f} refloated {} (station,sat) "
                  "restart response(s){} — the accumulator's own verdict; the step "
                  "is band-common (MW-blind), so no raw detector corroborates it "
                  "[{}]",
                  se.time.tow, n_resp,
                  carry_warm ? "" : " (carry cold; diagnostic)", detail);
}

void networkSlipDrainGapReentry(std::vector<NetStation>& stations,
                                const std::vector<bool>& present,
                                int i_lo, int n_sta_total,
                                bool carry_invariant, bool carry_warm,
                                std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                const RawSlipEpoch& raw, const SlipQcParams& qc,
                                const std::map<std::tuple<int, std::string, std::string>,
                                               LocalCarry>& local_carry,
                                double pl_now_s, double pl_max_gap_s,
                                SlipQcCounters& qc_cnt,
                                const SyncedEpoch& se, NetworkRunTraces& traces,
                                const std::shared_ptr<spdlog::logger>& log,
                                RelevelEpochState* relevel,
                                std::map<std::tuple<int, std::string, std::string>,
                                         RelevelPend>* relevel_pend) {
    if (!qc.gap_reentry) return;
    if (raw.resumptions.empty()) return;
    int n_resp = 0;
    std::string detail_log;
    // The resumption list is built station-ordered by the qualify pass, and this
    // walk touches only station i's own StateManager plus the two epoch-scoped
    // sets — serial, in that order, so it is deterministic.
    for (const auto& g : raw.resumptions) {
        const int i = g.station;
        if (i < 0 || i >= n_sta_total || i >= static_cast<int>(stations.size())) continue;
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        // --- the verdict, in the cause PRIORITY the counters are split on ----
        // A single re-entry can satisfy several gates (the field's poison case
        // is shifted AND LLI-flagged on both bands); the PRIMARY cause is the
        // strongest evidence, so the three freed counters partition the freed
        // population exactly while the event's `cause` field keeps all of them.
        const bool lli_set  = qc.gap_reentry_lli && (g.lli1 != 0 || g.lli2 != 0);
        const bool unmeas   = qc.gap_reentry_unmeasurable && !g.measurable;
        if (!g.shifted && !lli_set && !unmeas) continue;   // clean re-entry: bridge it, as designed
        ++qc_cnt.gap_reentry_verdicts;
        // The legacy pivot's ambiguities are GAUGE-PINNED: refloating one would
        // move the frame every emitted phase bias references, which is the
        // datum-slip re-reference path (MW drain), not this one.  Skipped and
        // counted rather than silently dropped.
        if (i < i_lo) { ++qc_cnt.gap_reentry_pivot_skipped; continue; }
        std::string cause_txt;
        std::uint8_t cause_bits = RelevelEpochState::kCauseGap;
        {
            const auto add = [&cause_txt](const char* t) {
                if (!cause_txt.empty()) cause_txt += '|';
                cause_txt += t;
            };
            if (g.shifted) add("shifted");
            if (lli_set)   add("lli");
            if (unmeas)    add("unmeas");
        }
        const char* primary = g.shifted ? "shifted" : (lli_set ? "lli" : "unmeas");
        StateManager& sm = *stations[static_cast<std::size_t>(i)].sm;
        bool voided_pends = false;
        for (const std::string& sig : {g.sig1, g.sig2}) {
            if (sig.empty()) continue;
            // ⚠ THE KEY IS SYNTHESISED, NOT LOOKED UP — see the header banner.
            // The first cut mirrored the MW/LLI drain's refloat_signal and
            // walked the live ambiguity map.  That mirror is structurally wrong
            // for a RE-ENTRY: the arc left the model set, so its ambiguity
            // column was pruned with the gap and PASS B's measurement pre-pass
            // (MeasurementModel::ensureRequiredStates) re-creates it only AFTER
            // this drain.  The walk matched nothing, and a 12 h field arm came
            // back byte-identical to its control with 396 SLIPPED verdicts and
            // 0 freed.  This is the key format the PASS-B inject site reads and
            // the one the Card-G arc classification synthesises, so a freed arc
            // reads `shifted_fresh` there by construction.
            const std::string amb_key = StateKeys::makeAmbiguityKey(g.satellite, sig);

            // "Is there anything to protect?" is a question about the CARRY.
            // A LocalCarry entry still inside the max-gap window is exactly
            // what the PASS-B inject would fold across the break — the same
            // `pl_bridged` predicate the Card-G classification applies, so the
            // instrument and the response cannot disagree about the population.
            bool pl_bridged = false;
            {
                const auto cit = local_carry.find(std::make_tuple(i, g.satellite, sig));
                if (cit != local_carry.end()) {
                    const double gap_s = pl_now_s - cit->second.last_seen_s;
                    pl_bridged = (gap_s >= 0.0 && gap_s <= pl_max_gap_s);
                }
            }
            // Under carry_invariant the DD chord test would need
            // NetworkInvariantCarryDriver, a runNetwork local that does not
            // reach this seam (ctx carries carry_sat_vertex / carry_group_id,
            // not the driver).  So every re-entry is treated as PROTECTABLE
            // there — the conservative direction: the C3c funnel insert is
            // idempotent and an arc with no carried chord re-taps fresh, which
            // is what it would have done anyway.  The cost is that
            // gap_reentry_no_carry cannot separate the two on that vehicle; the
            // DD ledger (gap_shifted_bridged / _fresh) still can, from the driver.
            if (!(pl_bridged || carry_invariant)) {
                ++qc_cnt.gap_reentry_no_carry;
                continue;
            }
            ++qc_cnt.gap_reentry_arcs;

            // Does the state exist YET?  Usually NOT — that IS the defect, so it
            // is counted rather than assumed.  The free-epoch latch is keyed on
            // the string and survives to the inject either way; only the
            // pending-reseed mark needs a live state, because a mark on an
            // absent key would sit in the set and be consumed by some later
            // epoch's pre-pass — a response acting on history.
            const bool state_live = sm.getAmbiguities().count(amb_key) != 0;
            if (!state_live) ++qc_cnt.gap_reentry_state_absent;

            // Card 4 C3c: the DD invariant carry owns its own funnel and is
            // live from epoch 1 (refloat_signal's contract, unchanged).
            if (carry_invariant) {
                carry_slipped_ambs.insert({i, amb_key});
                ++qc_cnt.gap_reentry_dd_marked;
            }
            if (carry_warm) {              // cold: nothing stale to refloat (counted below)
                if (state_live) sm.markAmbiguityPendingReseed(amb_key);
                if (relevel) {
                    relevel->free_ambs.insert({i, amb_key});
                    relevel->free_cause[{i, amb_key}] |= cause_bits;
                    ++qc_cnt.relevel_free_marked;
                }
            }
            if (!carry_warm) {
                ++qc_cnt.gap_reentry_cold_skipped;
            } else if (!relevel && carry_invariant) {
                // The leveling local carry is superseded and structurally EMPTY
                // under carry_invariant (the loader disarms relevel_free_epoch
                // there for exactly that reason), so the C3c funnel insert above
                // IS the whole response and nothing is missing.  A separate
                // bucket from `no_relevel`: one is by design, the other is a
                // response that could not land.
                ++qc_cnt.gap_reentry_dd_only;
            } else if (!relevel) {
                // The mark alone never delivered the free epoch (PASS-B consumes
                // it pre-inject) — say so instead of reporting a response that
                // cannot land.
                ++qc_cnt.gap_reentry_no_relevel;
            } else if (g.shifted) {
                ++qc_cnt.gap_reentry_freed_shifted;
            } else if (lli_set) {
                ++qc_cnt.gap_reentry_freed_lli;
            } else {
                ++qc_cnt.gap_reentry_freed_unmeas;
            }
            // One event per ARC reached (invariant #7's re-coupling edge needs
            // an identity, not just a count).  `statistic` is the raw ΔMW in
            // metres when the break was measurable and BLANK otherwise — a
            // fabricated 0.0 would read as "the level did not move", the exact
            // absence-of-evidence claim Card G exists not to make.
            traces.emitEvent(se.time, "T0_gap", "reentry_relevel", g.satellite,
                             stations[static_cast<std::size_t>(i)].name,
                             g.measurable ? std::optional<double>(g.d_mw) : std::nullopt,
                             std::nullopt,
                             (carry_warm && relevel)
                                 ? "refloat"
                                 : (!carry_warm ? "cold"
                                                : (carry_invariant ? "dd_only" : "no_relevel")),
                             fmt::format("{};gap_ep={};dGF={};lli1={};lli2={};cause={};"
                                         "carry={};state={}",
                                         sig, g.gap_epochs,
                                         g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                      : std::string{},
                                         g.lli1, g.lli2, cause_txt,
                                         // WHY this arc qualified as protectable, and
                                         // whether the column existed yet: the two facts
                                         // the first cut got wrong, per event.
                                         pl_bridged ? "pl" : "dd",
                                         state_live ? "live" : "absent"));
            // A fresh mark on this (station, sat) makes any outstanding bridge
            // pend's pre-slip capture STALE — the same rule the raw-slip drains
            // apply, for the same reason (its replace would re-inject a level
            // from before the break).  Once per resumption, not per band.
            if (carry_warm && relevel_pend && !voided_pends) {
                voided_pends = true;
                for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
                    if (std::get<0>(pit->first) == i
                        && std::get<1>(pit->first) == g.satellite) {
                        ++qc_cnt.bridge_pend_voided;
                        pit = relevel_pend->erase(pit);
                    } else ++pit;
            }
            if (++n_resp <= 12)
                detail_log += fmt::format("{}{}/{}/{} [{}]",
                                          detail_log.empty() ? "" : ", ",
                                          stations[static_cast<std::size_t>(i)].name,
                                          g.satellite, sig, primary);
        }
    }
    if (n_resp > 0)
        log->info("Network[gap-reentry]: TOW {:.1f} marked {} arc(s) slipped at re-entry{} "
                  "— no time-difference detector can run on a return epoch, so this is the "
                  "ONLY response between a shifted/LLI'd re-entry and the zero-q leveling "
                  "carry bridging the break [{}]",
                  se.time.tow, n_resp,
                  carry_warm ? (relevel ? "" : " (relevel_free_epoch OFF; mark cannot deliver "
                                               "the free epoch — counted, not acted)")
                             : " (carry cold; diagnostic)",
                  detail_log);
}

} // namespace GnssProcessor

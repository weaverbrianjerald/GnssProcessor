/**
 * @file NetworkSlipQc.cpp
 * @brief Serial slip-QC drains of the network run loop: the phase-leveling T0
 *        repair drain, the MW-detect / LLI refloat drain with its delta-bridge
 *        kernels, the IF-phase triple-difference driver, the wind-up restart
 *        drain, the injected arc-free drain and the gap re-entry drains.
 *
 * The first three drains were moved here from the SB_SLIP block of
 * ApplicationNetwork.cpp at decomposition INC-6 (see
 * docs/network_decomposition_plan.md).  The PASS-A parallel scan that fills
 * PassABuffers, and the calls into the drains below, live in
 * Application::networkPassASlipScan_ (ApplicationNetworkAssembly.cpp, INC-7);
 * the SB_SLIP D0b timer bracket in ApplicationNetwork.cpp wraps that call.
 * The three relocated drains open with a short alias block that binds the
 * former runNetwork() local names (buffer slots, trace stream, event
 * forwarder), so the relocated code reads with its original names.
 */

#include "network/slip/NetworkSlipQc.hpp"

#include "common/GnssIdentity.hpp"      // obsTypeOf, systemOfSat, SignalId, LosId
#include "config/GnssConfig.hpp"        // networkCycleSlip* getters
#include "filter/StateKeySchema.hpp"    // StateKeys::parseAmbiguityKey (refloat lambdas)
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch (se.time in log/event rows)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include "network/driver/NetworkRun.hpp"
#include "network/state/NetworkRunContext.hpp"
#include "network/driver/NetworkSetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/NetworkRunTraces.hpp"
#include "logging/LogManager.hpp"
#include "common/PppUtils.hpp"

namespace GnssProcessor {

void networkSlipDrainT0Repairs(const PassABuffers& pa, const GnssConfig& config_,
                               const std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               net::ArcTable& arcs,
                               const SyncedEpoch& se,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-6): the relocated body reads the PASS-A buffer under its
    // former local name.
    const auto& sta_repairs = pa.sta_repairs;
    // Phase-leveling T0, applied serially in station order (deterministic) before
    // PASS B reads the carry to build the prior.
    //   • Repair: a confident integer of sane magnitude (frac ≤ frac_max and
    //     |dN| ≤ dn_max) shifts the carried value continuously
    //     (value_abs += dN·λ; variance unchanged, covariance preserved).
    //   • Leave: a less confident emitted repair (rejected on frac or magnitude)
    //     is not acted on.  This is the data-driven choice for the BIA-less
    //     network: the float wide-lane's low-confidence repairs are mostly false
    //     positives, so acting on them (refloat or drop) discards a good carry
    //     and measurably degrades the products, while leaving them matches
    //     carry-only.  The lock-in risk from a rare real slip rejected here is
    //     bounded by phase_leveling_var_floor_m2 and the max-gap drop, and is
    //     left to the cross-station RAIM consensus (Increment 3), which can tell
    //     a real slip from a single-station false positive; a per-station T0
    //     cannot.  External BIA in the network MW would make the wide-lane
    //     integer reliable enough to turn this branch into a refloat.
    // The resolver's `reset_sats` is not acted on either: without BIA it is a
    // liberal catch-all of tens of satellites per epoch.  The loop starts at
    // i_lo: on the legacy datum (i_lo = 1) the pivot stations[0], whose
    // gauge-pinned ambiguities belong to Increment 4, is not touched; under the
    // prioritized datum i_lo = 0 and station 0 is processed like any other.
    int n_t0_repairs = 0, n_t0_left = 0;
    std::string t0_detail;
    const double t0_frac_max = config_.networkCycleSlipTurboEditFracMax();
    const int    t0_dn_max   = config_.networkCycleSlipTurboEditDnMax();
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        for (const auto& r : sta_repairs[i]) {
            if (r.dN == 0 || !(std::isfinite(r.lambda) && r.lambda > 0.0)) continue;
            auto it = net::getArc(arcs, std::make_tuple(i, r.satellite, r.signal), &net::ArcState::amb);
            if (it == nullptr) continue;
            const int adn = (r.dN < 0) ? -r.dN : r.dN;
            if (r.frac <= t0_frac_max && adn <= t0_dn_max) {
                it->value_abs += static_cast<double>(r.dN) * r.lambda;  // repair
                ++n_t0_repairs;
                if (n_t0_repairs <= 12)
                    t0_detail += fmt::format("{}{}/{}/{} dN={} f={:.2f}",
                                             t0_detail.empty() ? "" : ", ",
                                             stations[i].name, r.satellite, r.signal,
                                             r.dN, r.frac);
            } else {
                ++n_t0_left;  // leave (untrusted integer, likely a float-MW false positive)
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
    // Band frequencies from the two wavelengths alone (exact per satellite,
    // GLONASS FDMA included): 1/λ_WL = (f1−f2)/c, 1/λ_IF = (f1+f2)/c.
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / lambda_wl_m + 1.0 / lambda_if_m);
    const double f2 = 0.5 * c * (1.0 / lambda_if_m - 1.0 / lambda_wl_m);
    if (!(f2 > 0.0) || !(f1 > f2)) return r;
    const double lam1 = c / f1, lam2 = c / f2;
    // δ_IF = β1·ΔN1 − β2·ΔN2 with β_j = λ_WL·f_j/(f1+f2); β1 − β2 ≡ λ_IF.
    const double beta1 = lambda_wl_m * f1 / (f1 + f2);
    // Float solve (a bridge, never an integer fix):
    //   ΔN2 = (δ_IF − β1·ΔN_WL)/λ_IF;   ΔN1 = ΔN2 + ΔN_WL.
    // (The β1·ΔN_WL/λ_IF term is this kernel's counterpart of the GF kernel's
    //  documented λ1-in-numerator error; an integer-injection test covers it.)
    const double dn2 = (dif_m - beta1 * dn_wl_cyc) / lambda_if_m;
    const double dn1 = dn2 + dn_wl_cyc;
    if (!std::isfinite(dn1) || !std::isfinite(dn2)) return r;
    if (std::abs(dn1) > max_abs_cyc || std::abs(dn2) > max_abs_cyc) return r;
    // Variance propagation (independent σ_IF [m], σ_WL [cyc]).  The 1/λ_IF²
    // factor is an amplification of about 9× in σ, which the bridged prior carries.
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
    // Band frequencies from the two wavelengths alone, as in the v2 kernel
    // (exact per satellite, GLONASS FDMA included).
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    if (!(f2 > 0.0) || !(f1 > f2)) return r;
    const double lam1 = c / f1, lam2 = c / f2;
    const double beta1 = pd.lambda_wl * f1 / (f1 + f2);
    const double beta2 = beta1 - pd.lambda_if;

    double dn1, dn2, v1, v2;
    if (pd.has_dn) {
        // With a WL integer (the E2 route): the v2 closed form, same math as
        // networkSlipDeltaBridgeSolve.
        if (!std::isfinite(pd.dif_m) || !std::isfinite(pd.sigma_if_m)) return r;
        dn2 = (pd.dif_m - beta1 * pd.dn_wl) / pd.lambda_if;
        dn1 = dn2 + pd.dn_wl;
        v2  = (pd.sigma_if_m * pd.sigma_if_m
               + beta1 * beta1 * pd.sigma_wl * pd.sigma_wl)
              / (pd.lambda_if * pd.lambda_if);
        v1  = (pd.sigma_if_m * pd.sigma_if_m
               + beta2 * beta2 * pd.sigma_wl * pd.sigma_wl)
              / (pd.lambda_if * pd.lambda_if);
        // Over-determination screen with the GF equation (E4): a wrong
        // detector-rounded WL integer (±1 cyc) sits 2.5–5σ off the E1+E4 float
        // plane.  The propagated σ cannot show it, because propagation carries
        // measurement noise and not the risk of a wrong integer (the mw_only
        // lesson); the plane residual can.  Above kBridgePlaneResidMax the
        // bridge is rejected.
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
        // Without a WL integer, solve the IF and GF equations (the E1+E4 route):
        // [β1 −β2; λ1 −λ2]·ΔN = [δIF; dGF], with full 2×2 covariance propagation
        // and the shared-noise off-diagonal ρ·σIF·σGF.
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
        r.used_gf = true;
    } else {
        return r;   // no second equation yet: keep pending
    }
    if (!std::isfinite(dn1) || !std::isfinite(dn2)) return r;
    if (std::abs(dn1) > max_abs_cyc || std::abs(dn2) > max_abs_cyc) return r;
    if (!(v1 >= 0.0) || !(v2 >= 0.0)) return r;
    if (std::sqrt(v1) > max_sigma_cyc || std::sqrt(v2) > max_sigma_cyc) return r;

    r.ok = true;
    r.dn1_cyc = dn1;  r.dn2_cyc = dn2;
    r.shift1_m = lam1 * dn1;  r.shift2_m = lam2 * dn2;
    r.var1_m2 = lam1 * lam1 * v1;  r.var2_m2 = lam2 * lam2 * v2;
    return r;
}

BridgeReplaceOutcome networkSlipBridgeV3Replace(
        const RelevelPend& pd, int sta, const std::string& sat,
        net::ArcTable& arcs,
        double rho_if_gf, double max_abs_cyc, double max_sigma_cyc,
        double corrob_cyc, BridgeV3Solve* solved) {
    const auto bs = networkSlipBridgeV3Solve(pd, rho_if_gf,
                                             max_abs_cyc, max_sigma_cyc);
    if (solved) *solved = bs;
    if (!bs.ok) return BridgeReplaceOutcome::KernelReject;
    auto it1 = net::getArc(arcs, {sta, sat, pd.sig1}, &net::ArcState::amb);
    auto it2 = net::getArc(arcs, {sta, sat, pd.sig2}, &net::ArcState::amb);
    if (it1 == nullptr || it2 == nullptr)
        return BridgeReplaceOutcome::NoCarry;
    // Per-band wavelengths for the cycle-domain corroboration.
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    const double lam1 = c / f1, lam2 = c / f2;
    const double L1 = pd.pre1 + bs.shift1_m;
    const double L2 = pd.pre2 + bs.shift2_m;
    if (std::abs(L1 - it1->value_abs) / lam1 > corrob_cyc ||
        std::abs(L2 - it2->value_abs) / lam2 > corrob_cyc)
        return BridgeReplaceOutcome::CorrobReject;
    it1->value_abs = L1;  it1->var = pd.pre_var1 + bs.var1_m2;
    it2->value_abs = L2;  it2->var = pd.pre_var2 + bs.var2_m2;
    return BridgeReplaceOutcome::Applied;
}

BridgeReplaceOutcome networkSlipBridgeReplace(
        const RelevelPend& pd, int sta, const std::string& sat,
        net::ArcTable& arcs,
        double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
        DeltaBridgeSolve* solved) {
    const auto bs = networkSlipDeltaBridgeSolve(pd.dn_wl, pd.sigma_wl,
                                                pd.dif_m, pd.sigma_if_m,
                                                pd.lambda_wl, pd.lambda_if,
                                                max_abs_cyc, max_sigma_cyc);
    if (solved) *solved = bs;
    if (!bs.ok) return BridgeReplaceOutcome::KernelReject;
    auto it1 = net::getArc(arcs, {sta, sat, pd.sig1}, &net::ArcState::amb);
    auto it2 = net::getArc(arcs, {sta, sat, pd.sig2}, &net::ArcState::amb);
    if (it1 == nullptr || it2 == nullptr)
        return BridgeReplaceOutcome::NoCarry;
    // Per-band wavelengths for the cycle-domain corroboration (same derivation
    // as the kernel's).
    constexpr double c = 299792458.0;
    const double f1 = 0.5 * c * (1.0 / pd.lambda_wl + 1.0 / pd.lambda_if);
    const double f2 = 0.5 * c * (1.0 / pd.lambda_if - 1.0 / pd.lambda_wl);
    const double lam1 = c / f1, lam2 = c / f2;
    const double L1 = pd.pre1 + bs.shift1_m;
    const double L2 = pd.pre2 + bs.shift2_m;
    // Corroboration (§8.1): the bridged level must agree with the interim v1
    // free re-level to within corrob_cyc.  Correct bridges err <= 0.5 cyc
    // against truth and the free level sits within ~2 cyc, while wrong bridges
    // erred 6–113 cyc in the audit, so agreement separates the populations.
    if (std::abs(L1 - it1->value_abs) / lam1 > corrob_cyc ||
        std::abs(L2 - it2->value_abs) / lam2 > corrob_cyc)
        return BridgeReplaceOutcome::CorrobReject;
    // Replace value and var only (the local-step mutation rule).  The coupling
    // regression and last_seen belong to the current harvest, and a pure level
    // re-base leaves both valid.
    it1->value_abs = L1;  it1->var = pd.pre_var1 + bs.var1_m2;
    it2->value_abs = L2;  it2->var = pd.pre_var2 + bs.var2_m2;
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
                               net::ArcTable* datum_arcs,
                               RelevelEpochState* relevel,
                               std::map<std::tuple<int, std::string, std::string>,
                                        RelevelPend>* relevel_pend,
                               net::ArcTable* bridge_arcs,
                               NetworkSlipOracle* oracle,
                               const CommonBandEpoch* common_band) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_mw_refloat = pa.sta_mw_refloat;
    // Sim-only oracle: this epoch's continuous GPS seconds, the time frame the
    // truth table is indexed in (the same expression as the wind-up drain's now_s).
    const double oracle_now_s =
        static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
    // Thin event forwarder, a textual copy of the run-TU lambda, so the moved
    // emitEvent call sites are unchanged (same rows, same order).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // MW detector (mw_detect) → refloat: the iono-safe complement to the IF-phase TD.
    // Two signals, neither of which uses the geometry-free ionosphere, so both stay
    // valid at the 30 s product rate (unlike the TurboEdit MW+GF integer repair):
    //   (a) a confident wide-lane integer jump collected in PASS A (sta_mw_refloat);
    //   (b) the receiver's own LLI loss-of-lock flag (bit 0) on a carried signal,
    //       which the receiver sets on a cycle slip or re-acquisition and which
    //       also marks real slips that MW and IF cannot see.
    // Either one refloats the affected ambiguity (marks it pending-reseed so PASS B
    // re-estimates it from this epoch's data) and never applies an integer, so a
    // wrong wide-lane integer cannot bias the gauge.  Serial and station-ordered,
    // hence deterministic.  The refloat matters only when the carry is warm; cold,
    // the detection is diagnostic (the free per-epoch estimate needs no reseed).
    // On the legacy pivot datum refloat_signal never touches stations[0], whose
    // ambiguities are gauge-pinned: the station loop starts at i_lo = 1, or, with
    // datum_station_detect armed, station 0's detections take the network
    // re-reference path below.  Under the prioritized datum (i_lo = 0) station 0
    // is refloated like any other station.
    if (config_.networkCycleSlipMwDetect()) {
        // Matched (a detection landed on a live ambiguity) and refloated (the warm
        // carry was reset) are recorded separately (Card E).  A cold run matches,
        // and feeds the C3c funnel, without refloating; the separate flag lets it
        // still report its MW/LLI events, with action "diagnostic" (limitation L3).
        struct SignalAction { bool matched{false}; bool refloated{false};
            // Sim-only oracle (network.cycle_slip.oracle_relevel): the arc was
            // handled by a truth-fed level repair (or a truth-fed false-alarm
            // keep) instead of being freed.  `refloated` stays false, since
            // nothing was refloated, so every downstream gate that keys on it (the
            // bridge candidate push, the refloat counters) is bypassed; the event
            // row's action is taken from these fields instead.
            bool oracle_handled{false}; bool oracle_repaired{false};
            std::string oracle_detail; };
        // `cause` is the freeing-cause bit, §7.6(c)
        // (RelevelEpochState::kCauseMw / kCauseLli): a classification tag, OR-merged
        // so that a key freed twice keeps both bits.
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
                    // Sim-only oracle slip response (include/network/slip/NetworkSlipOracle.hpp):
                    // a perfect repair replaces the free.  When the oracle handles
                    // the arc, every freeing action below is skipped (C3c funnel
                    // insert, pending-reseed mark, free_ambs latch, bridge-pend
                    // void) and only the event row remains, with an oracle action.
                    // `oracle` is null unless the sim-only knob is armed.
                    if (oracle) {
                        OracleOutcome oc = OracleOutcome::NotHandled;
                        std::string det;
                        // `cause` is this site's own tier bit (kCauseMw for the
                        // wide-lane drain, kCauseLli for the receiver's LLI); it
                        // affects only the attribution of a false-alarm keep.
                        if (networkSlipOracleHandle(*oracle, i, sat, signal,
                                                    oracle_now_s, cause, det, oc)) {
                            act.oracle_handled  = true;
                            act.oracle_repaired = (oc == OracleOutcome::Repaired);
                            act.oracle_detail   = det;
                            continue;
                        }
                    }
                    // Feed the DD invariant carry from epoch 1, independent of the
                    // legacy carry's warm gate (Card 4 C3c).  The DD carry is live
                    // throughout, and pending-reseed cannot serve it: the PASS-B
                    // MeasurementModel consumes and clears the mark before the fold.
                    if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                    // Legacy phase-leveling refloat: only meaningful once warm.
                    if (carry_warm) { sm.markAmbiguityPendingReseed(amb_key); act.refloated = true;
                        // The mark alone does not deliver the free epoch, because
                        // PASS B consumes it before the inject; also latch the set
                        // the inject site reads (SPEC_slip_relevel_walk).
                        if (relevel) {
                            relevel->free_ambs.insert({i, amb_key});
                            relevel->free_cause[{i, amb_key}] |= cause;
                            ++qc_cnt.relevel_free_marked;
                        }
                        // Bridge v2: a fresh mark on this (station, sat) makes the
                        // pre-slip capture of any outstanding pend stale, so void it
                        // (the T1 flag path re-creates its own pend after this).
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
        // The LLI qualification (Card L) is armed only when the toggle is on and
        // this epoch has raw evidence.  Without evidence an LLI stays decisive: a
        // gate that fires on missing data would do more harm than the LLI chatter
        // it is meant to filter.
        const bool lli_qual = qc.lli_corroborate && raw.computed;

        // Datum-station slip detect, §9.7.10 (network.cycle_slip.datum_station_detect):
        // armed iff the caller passed the queues (legacy pivot datum only).  A slip
        // detected at station 0 cannot be answered with refloat_signal: its
        // ambiguities are gauge-pinned, and the level that every emitted phase bias
        // of that satellite references has just moved.  The response is a network
        // re-reference, split across the two edges that invariant #7 names:
        //   queue edge (here, at detection): sat → pb queue (this epoch's stash
        //   skips its PhaseBias carry entirely) + amb queue + datum_slip
        //   event/count;
        //   apply edge (the block below, at the first drain after that skip-stash
        //   was consumed): all-station ambiguity reseed + local-carry erase + disc
        //   mark + datum_reref event/count, so the integers re-reference in the
        //   same solve in which the bias re-seeds.
        const bool datum_active = datum_pb_reref_sats && datum_amb_reref_sats
                                  && datum_mark_sats;
        auto datum_slip_detect = [&](const std::string& sat, const std::string& detail) {
            const bool fresh = datum_amb_reref_sats->insert(sat).second;
            datum_pb_reref_sats->insert(sat);
            if (!fresh) return;   // already queued and not yet applied: dedupe
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
        // withheld, so the stash never consumed the skip) stays deferred, so the
        // two edges cannot come apart across withholds.  Station 0 itself is not
        // reseeded: its pin at the new phase level is the new reference.  Absent
        // stations are reseeded too: a pending-reseed mark persists until that
        // station's PASS B next runs, which is its re-coupling edge.
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
                        if (datum_arcs
                            && net::dropArc(*datum_arcs, std::make_tuple(i, parts->satellite, parts->signal), &net::ArcState::amb))
                            ++qc_cnt.datum_slip_carry_erases;   // retirement census (D)
                        ++n_marked;
                    }
                }
                datum_mark_sats->insert(sat);
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
        // and replace the ones whose MW integer arrived in the same epoch as the
        // T1 flag (their corroborating free re-level was harvested last epoch).
        const bool bridge_v2 = relevel_pend && bridge_arcs
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
                pd, rsta, rsat, *bridge_arcs,
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
        // Bridge v3 dispatch: when the GF rider is armed, every replace routes
        // through the v3 kernel (identical math to v2 on an E2-only pend, plus
        // the plane-residual screen when the GF row is present).
        const bool bridge_v3_gf  = bridge_v2 && config_.networkCycleSlipRelevelBridgeGf();
        auto v3_replace = [&](const std::tuple<int, std::string, std::string>& key,
                              const RelevelPend& pd) {
            BridgeV3Solve bs;
            const int rsta = std::get<0>(key);
            const std::string& rsat = std::get<1>(key);
            const auto outcome = networkSlipBridgeV3Replace(
                pd, rsta, rsat, *bridge_arcs, kBridgeGfIfRho,
                v2_max_cyc, v2_max_sig, v2_corrob, &bs);
            switch (outcome) {
                case BridgeReplaceOutcome::Applied: {
                    ++qc_cnt.bridge_pairs_applied;
                    if (bs.used_gf) ++qc_cnt.bridge_gf_applied;
                    qc_cnt.bridge_max_abs_dn_cyc =
                        std::max({qc_cnt.bridge_max_abs_dn_cyc,
                                  std::abs(bs.dn1_cyc), std::abs(bs.dn2_cyc)});
                    emitEvent("T1_if_slip", "bridge_replace", rsat,
                              stations[rsta].name, bs.dn1_cyc, bs.dn2_cyc, "bridge",
                              fmt::format("{};{};lag_s={:.0f};sd1={:.3f};sd2={:.3f};"
                                          "route={}",
                                          pd.sig1, pd.sig2, v2_now_s - pd.tow_s,
                                          std::sqrt(bs.var1_m2), std::sqrt(bs.var2_m2),
                                          bs.used_gf ? "gf" : "mw"));
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
            return bridge_v3_gf ? v3_replace(key, pd) : v2_replace(key, pd);
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
            // band but count, log and emit once per (station, satellite).
            std::set<std::string> mw_done;
            for (const auto& cand : sta_mw_refloat[i]) {
                const auto& sat  = cand.sat;
                const auto& sig1 = cand.sig1;
                const auto& sig2 = cand.sig2;
                if (datum_active && i == 0) {
                    // Datum station: a confident WL integer jump takes the network
                    // re-reference path, not refloat_signal (pinned ambiguities).
                    if (mw_done.insert(sat).second)
                        datum_slip_detect(sat, "MW;" + sig1 + ";" + sig2);
                    continue;
                }
                // Bridge v2 intercept, before the mark: the belated MW integer for
                // a slip that T1 already flagged (and freed) 1-2 epochs ago.  Fill
                // the waiting pend and replace immediately (the corroborating free
                // re-level was harvested at least one epoch ago).  On Applied the
                // normal mark/free path is skipped: the slip's event already fired
                // at the flag epoch, and a second free would discard the level just
                // replaced.  Every other outcome falls through to the normal path
                // (the arc is freed again, the conservative choice).
                //
                // MW evidence refloat: an evidence-admitted candidate may carry no
                // usable integer (dn_wl is 0 and meaningless there).  Every branch
                // below consumes cand.dn_wl as a measured step, so the whole
                // intercept is skipped for those candidates: filling has_dn with a
                // zero would bridge the carried level by zero, i.e. assert that the
                // slip did not move it, which is worse than not bridging at all.
                // Byte-identical with the knob off: the legacy gate requires
                // slip_wl_known, so integer_known is true for every candidate it
                // admits.
                if (bridge_v2 && cand.integer_known) {
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
                            continue;   // skip the second free: the level was replaced
                        }
                    }
                }
                // Common-band scope (§14): this pair may free only those of its
                // bands that the epoch's single verdict implicated.  With a null
                // verdict (knob off), or no narrowed verdict for this (station,
                // satellite), both calls return true and the unscoped path runs.
                const bool cb1 = commonBandMayFree(common_band, i, sat, sig1);
                const bool cb2 = commonBandMayFree(common_band, i, sat, sig2);
                const auto a = !cb1 ? SignalAction{}
                             : refloat_signal(i, sat, sig1,
                                              RelevelEpochState::kCauseMw);
                const auto b = !cb2 ? SignalAction{}
                             : refloat_signal(i, sat, sig2,
                                              RelevelEpochState::kCauseMw);
                // Bridge v2: a same-epoch candidate (MW and T1 firing together) is
                // deferred to the IF-TD driver, which stores the integer in the
                // pend it creates this epoch; the replace then runs at the next
                // epoch's sweep, once the corroborating free re-level exists.
                // cand.integer_known is required for the same reason as in the
                // intercept above: the deferred path replaces from the stored
                // dn_wl, so an unknown integer must not enter (byte-identical with
                // the knob off, see the intercept comment).
                if (relevel && carry_warm && config_.networkCycleSlipRelevelDeltaBridge()
                    && cand.integer_known && (a.refloated || b.refloated))
                    relevel->mw_cands.push_back(MwBridgeCand{
                        i, sat, sig1, sig2, cand.dn_wl,
                        std::max(cand.frac,
                                 config_.networkCycleSlipRelevelBridgeWlSigmaFloorCyc()),
                        cand.lambda_wl});
                const bool matched   = a.matched   || b.matched;
                const bool refloated = a.refloated || b.refloated;
                // Sim-only oracle: the detection is unchanged (same population,
                // same row) and only the action and the detail differ, so an
                // oracle arm and its control compare directly in network_events.csv.
                const bool orc_h = a.oracle_handled || b.oracle_handled;
                const bool orc_r = a.oracle_repaired || b.oracle_repaired;
                // The event is one row per satellite but the oracle decides per
                // band, so the detail comes from the band the action names; an
                // "oracle_repair" row carrying the other band's "delta=0;n=0"
                // would contradict itself.
                const std::string orc_detail =
                    a.oracle_repaired ? a.oracle_detail
                  : b.oracle_repaired ? b.oracle_detail
                  : a.oracle_handled  ? a.oracle_detail : b.oracle_detail;
                // MW evidence refloat: an evidence-admitted row carries its
                // provenance, so the population can be separated in the trace
                // without re-deriving the rule.  Tier, event and action are
                // unchanged (it is the same detection, admitted by a second gate),
                // and legacy rows are byte-identical.
                const std::string ev_detail =
                    cand.via_evidence
                        ? fmt::format(";src=evidence;ratio={:.1f};n={};int={}",
                                      cand.evidence_ratio, cand.arc_epochs,
                                      cand.integer_known ? "known" : "unknown")
                        : std::string{};
                if (matched && mw_done.insert(sat).second) {
                    emitEvent("T0_mw_detect", "mw_wl_slip", sat, stations[i].name,
                              std::nullopt, std::nullopt,
                              orc_h ? (orc_r ? "oracle_repair" : "oracle_keep")
                                    : (refloated ? "refloat" : "diagnostic"),
                              (orc_h ? (sig1 + ";" + sig2 + ";" + orc_detail)
                                     : (sig1 + ";" + sig2))
                                  + ev_detail);
                    ++qc_cnt.mw_detections;
                    if (refloated) ++qc_cnt.mw_refloats;
                    if (++n_mw <= 12)
                        mw_detail += fmt::format("{}{}/{}", mw_detail.empty() ? "" : ", ",
                                                 stations[i].name, sat);
                }
            }
            // (b) receiver LLI loss-of-lock (bit 0) on any carried phase signal.
            // modeled[i] holds one entry per satellite; refloat every LLI'd band but
            // count and emit once per satellite (an LLI on both L1 and L2 is one slip).
            const bool credible =
                !lli_qual || (i < static_cast<int>(raw.lli_credible.size())
                              && raw.lli_credible[static_cast<std::size_t>(i)] != 0);
            for (const auto& obs : modeled[i]) {
                // The LLI qualification (Card L) is a per-(station, satellite)
                // decision, so first establish whether this satellite asserts LLI.
                bool asserts = false;
                for (const auto& [code, lli] : obs.lli_flags)
                    if (obsTypeOf(code) == ObsType::Phase && (lli & 1)) { asserts = true; break; }
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
                    // The shared observables are formed on the primary
                    // dual-frequency pair, so MW/GF cannot witness an LLI asserted
                    // only on a band outside that pair (e.g. L5 on an
                    // L1/L2-primary arc).  Such assertions are counted separately
                    // (lli_offpair) so that "uncorroborated" does not absorb
                    // "unwitnessable"; a nonzero off-pair count is the signal to
                    // widen the corroboration primitive to every tracked pair.
                    if (measurable) {
                        bool on_pair = false;
                        for (const auto& r : raw.rows[static_cast<std::size_t>(i)]) {
                            if (r.satellite != obs.satellite) continue;
                            for (const auto& [code, lli] : obs.lli_flags)
                                if (obsTypeOf(code) == ObsType::Phase && (lli & 1)
                                    && (code == r.sig1 || code == r.sig2)) { on_pair = true; break; }
                            break;
                        }
                        offpair = !on_pair;
                    }
                }
                const bool corroborated = corr_mw || corr_gf;
                // Act when the assertion is corroborated by the receiver's own
                // observables, which is decisive at any station, so a real
                // multi-satellite re-lock is acted on even at a non-credible
                // station (spec M3); or when the station's LLI is still credible
                // (spec M1).  Corroboration is not required everywhere, because
                // that would lose the slips that MW and IF cannot see.
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
                        // A suppressed assertion still gets its own event row (spec
                        // M4), carrying the station's credibility score so the
                        // top-offenders list can be rebuilt from the trace.
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
                    // Datum station: a qualified LLI assertion takes the network
                    // re-reference path.  The Card-L suppression above applies here
                    // too, so a chattering pivot receiver cannot re-reference the
                    // network repeatedly.
                    datum_slip_detect(obs.satellite,
                                      corr_mw ? "LLI;mw_step"
                                              : (corr_gf ? "LLI;gf_step" : "LLI"));
                    continue;
                }

                bool matched = false, refloated = false;
                bool orc_h = false, orc_r = false;
                std::string orc_detail;
                for (const auto& [code, lli] : obs.lli_flags) {
                    if (obsTypeOf(code) != ObsType::Phase || !(lli & 1)) continue;
                    // The §14 rule always implicates an LLI band, so this guard
                    // never fires on a band the receiver flagged.  It is here
                    // because the LLI loop and the MW loop must follow the same
                    // single verdict (the per-epoch-union invariant).
                    if (!commonBandMayFree(common_band, i, obs.satellite, code)) continue;
                    const auto sa = refloat_signal(i, obs.satellite, code,
                                                   RelevelEpochState::kCauseLli);
                    if (sa.matched)   matched   = true;
                    if (sa.refloated) refloated = true;
                    // Sim-only oracle: an LLI can be asserted on several bands of
                    // one satellite, and each is handled independently (a slip on
                    // L1C shifts only L1C).  The event stays one per satellite, so
                    // it carries the detail of the band its action names: the
                    // first repaired band, else the first handled one.
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
                           net::ArcTable* bridge_arcs,
                           SlipQcCounters* bridge_cnt,
                           std::map<std::tuple<int, std::string, std::string>,
                                    RelevelPend>* relevel_pend,
                           const RawSlipEpoch* raw_epoch,
                           const std::vector<RawSlipHistory>* raw_hist,
                           NetworkSlipOracle* oracle,
                           const CommonBandEpoch* common_band,
                           IfTdPairTallies* pair_tally) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_if_resets   = pa.sta_if_resets;
    auto&       if_slip_trace   = traces.if_slip_trace;
    // Thin event forwarder, a textual copy of the run-TU lambda (see above).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // IF-phase triple-difference slip detection and refloat.  In the PASS-A scan
    // each station's detector ran in deferred mode: it computed the
    // between-satellite, between-epoch ionosphere-free phase double differences
    // (ΔΔφ_IF) per (sat, signal pair) and cached them without flagging.  This
    // driver decides, elevation-aware and per (station, signal pair):
    //
    //  1. Centre: pool ΔΔφ_IF by signal pair across all present stations that have
    //     a detector, and take the median `med` (raw m).  That is the network
    //     common mode (frame / datum wobble); it does not depend on elevation, so
    //     it is removed in raw space.
    //  2. Scale: normalize each residual to a zenith equivalent, z = (dd−med)·
    //     sin(el) = (dd−med)/ifElevMap, so satellites at every elevation share one
    //     scale.  The pooled s_pool = 1.4826·median(|z|) is the warm-up prior; each
    //     (station, pair) also keeps an EWMA of its own z-scale (if_pair_scale).
    //     The effective scale shrinks from the pool toward the station's EWMA as
    //     that station-pair accumulates clean samples (empirical Bayes, weight
    //     count/(count+κ)), so a quiet reference station and a noisy modernized-
    //     signal station each get their own envelope instead of one pooled
    //     threshold set by the majority, which over-fires on L5Q/L2L.
    //  3. Threshold for sat i: max(k·s_eff·ifElevMap(el_i), floor_frac·λ_IF(1,1)),
    //     the empirical term re-inflated to sat i's elevation, with the physical
    //     (1,1)-integer floor kept in raw m so a real slip is caught at any
    //     elevation.  Flag if |dd_i − med| exceeds it.
    //  4. A flagged arc has its detector gradient reset (a fresh arc next epoch,
    //     whatever the carry state); under if_phase_td_reseed_on_flag the gradient is
    //     kept instead as the next epoch's origin, so that epoch is still evaluated
    //     (card ARL).  When the carry is warm, both band
    //     ambiguities are marked pending-reseed so PASS B refloats them (less any
    //     band the band scope or the common-band scope keeps).  Clean survivors
    //     then update the station-pair EWMA; slips are excluded, so the scale does
    //     not absorb the events it has to catch.
    //
    // The detector's own reference-satellite common-mode slip keys, which the
    // per-pair pool cannot see, are folded in too.  Thresholds are deliberately
    // loose; the sharper pass is left to a post-estimation phase validation that
    // is not built yet.  Serial and station-ordered, hence deterministic.  On the
    // legacy datum the pivot has no detector; under the prioritized datum every
    // station has one.
    if (config_.networkCycleSlipIfPhaseTd()) {
        const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
        const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
        const int    min_pool     = config_.networkCycleSlipIfPhaseTdMinPool();
        const double ewma_alpha   = config_.networkCycleSlipIfPhaseTdEwmaAlpha();
        const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

        // Pair width, §14.10 rule: `first` (default) is the pre-knob detector, one
        // pair per satellite (its first two distinct phase bands).  Every other
        // pair is then unwatched, and the equal-count case that the common-band
        // scope's IF-TD veto exists to close stays open on all of them.  `all`
        // evaluates every pair, each on its own gradient chain in the detector and
        // its own pool here (`by_pair` is already keyed by pair kind), so the first
        // pair's samples, centre, scale and thresholds are unchanged and the new
        // pairs are a pure addition.
        const bool iftd_all_pairs =
            config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All;

        // Post-flag re-seed (card ARL, slip-ladder defect 2): a flag keeps the
        // satellite's flag-epoch gradient instead of restarting its chain, so the next
        // epoch has a double difference (see step 4 below).
        const bool reseed_on_flag =
            config_.getNetworkConfig().slip_qc.if_phase_td_reseed_on_flag;

        // Sim-only oracle outcome for one flagged IF pair (see refloat_signal's
        // SignalAction).  `handled` is set when any band was handled; the pend gate
        // below uses it, because a bridge replace that re-bases one band while the
        // other was truth-repaired would overwrite the repair with a pre-slip level.
        struct PairAction { bool handled{false}; bool repaired{false};
                            bool freed{false}; std::string detail;
                            /// Bands of this pair that the common-band verdict
                            /// refused this epoch, per §14.12 and invariant #7 (an
                            /// identity at the edge, not only a counter): without
                            /// it a scoped pair refloat and an unscoped one would
                            /// write the same row.  Empty when nothing was refused;
                            /// the row then carries no token and keeps the pre-knob
                            /// text.
                            std::string cb_kept; };
        const double oracle_now_s =
            static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
        // Refloat both band ambiguities of (sat, sig1, sig2) at station i (only
        // when the carry is warm; cold, the detection is diagnostic only).
        auto refloat_pair = [&](int i, const std::string& sat,
                                const std::string& sig1,
                                const std::string& sig2) -> PairAction {
            PairAction pact;
            StateManager& sm = *stations[i].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                // Common-band scope, §14 rule: free (this pair's bands ∩ implicated).
                // With a null verdict (knob off) or no narrowed verdict the call
                // returns true and the unscoped path runs.
                if (!commonBandMayFree(common_band, i, sat, parts->signal)) {
                    // Record the refused band (the §14.12 token), but only for a
                    // band of this pair: the satellite's other bands were never
                    // this drain's to free.
                    if (parts->signal == sig1 || parts->signal == sig2) {
                        if (!pact.cb_kept.empty()) pact.cb_kept += "+";
                        pact.cb_kept += parts->signal;
                    }
                    continue;
                }
                if (parts->signal == sig1 || parts->signal == sig2) {
                    // Sim-only oracle: each band is handled independently (the IF
                    // combination cannot localize the slip, but truth can); a
                    // handled band skips every freeing action.
                    if (oracle) {
                        OracleOutcome oc = OracleOutcome::NotHandled;
                        std::string det;
                        if (networkSlipOracleHandle(*oracle, i, sat, parts->signal,
                                                    oracle_now_s,
                                                    RelevelEpochState::kCauseIfTd,
                                                    det, oc)) {
                            pact.handled = true;
                            // The event names one action for the pair, so its
                            // detail is that of the band the action describes: the
                            // first repaired band, else the first handled one.
                            if (oc == OracleOutcome::Repaired && !pact.repaired) {
                                pact.repaired = true;
                                pact.detail   = det;
                            } else if (pact.detail.empty()) {
                                pact.detail = det;
                            }
                            continue;
                        }
                    }
                    // Feed the DD carry from epoch 1 (Card 4 C3c; see refloat_signal).
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
                        // this (station, sat), whose pre-slip capture is now stale.
                        // The T1 flag path re-creates its own pend after this.
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

        // Pool this epoch's ΔΔφ_IF by signal pair across the present stations that
        // have a detector.  Each sample carries its elevation (for the stochastic
        // normalization) and the per-sat (1,1) IF wavelength (for the physical
        // floor, exact even for GLONASS FDMA).  is_ref marks the constellation
        // reference satellite, whose dd is identically 0 by construction; it is
        // carried per sample so the robust scale below can drop it (see §6.14.5
        // defect 2), and it stays in `samples` for the centring median and for
        // thresholding, so the flag population is unchanged.
        // `state_key` is the detector chain the sample rode (§14.10 rule), so a flag
        // restarts that chain alone (resetSatelliteInState) instead of every pair
        // of the satellite, which keeps the pair decisions independent.  Unused
        // under `first`, where there is one chain per system.
        // `after_reseed` marks the evaluation a re-seed restored (the epoch after a
        // flag; always false with the re-seed off).
        struct PairSample { int sta; std::string sat, sig1, sig2; double dd, el, lambda;
                            bool is_ref; std::string state_key; bool after_reseed; };
        std::map<std::string, std::vector<PairSample>> by_pair;
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i] || !stations[i].if_slip) continue;
            if (!iftd_all_pairs) {
                for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                    if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                    if (jp.sig1.empty() || jp.sig2.empty()) continue;
                    const std::string pair = jp.sig1 + "_" + jp.sig2;
                    by_pair[pair].push_back({i, sat, jp.sig1, jp.sig2,
                                             jp.delta_grad_m, jp.elevation_deg, jp.lambda_if_m,
                                             jp.is_reference, jp.state_key, jp.after_reseed});
                }
            } else {
                for (const auto& [key, jp] : stations[i].if_slip->getLastIfJumpsAllPairs()) {
                    if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                    if (jp.sig1.empty() || jp.sig2.empty()) continue;
                    by_pair[key.second].push_back({i, key.first, jp.sig1, jp.sig2,
                                                   jp.delta_grad_m, jp.elevation_deg,
                                                   jp.lambda_if_m,
                                                   jp.is_reference, jp.state_key,
                                                   jp.after_reseed});
                }
            }
        }

        int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;
        std::string if_detail;

        // --- Bridge v2, pend side: SPEC_slip_relevel_walk §8.2 -----------------
        // The flag epoch only captures: the pre-slip carry (this PASS-A map still
        // holds the previous harvest) and the measured δIF with its empirical σ,
        // since only this scope has (dd − med) and s_eff·emap.  The arc then takes
        // the v1 free epoch.  The replace (with the §8.1 corroboration against
        // that free level) runs in the MW drain when the confident WL integer
        // arrives, measured at 1–2 epochs later; a same-epoch MW candidate is
        // stored in the pend (cand_idx below) and replaced at the next epoch's
        // sweep.
        const bool bridge_on = relevel && bridge_arcs && bridge_cnt
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
        const double br_gf_floor  = config_.networkCycleSlipRelevelBridgeGfSigmaFloorM();
        for (auto& [pair, samples] : by_pair) {
            // (1) Network common-mode centre: median of the raw DDs (m).
            // (2) Pooled elevation-normalized robust scale (see below).
            // Both live in ifTdPoolStats (NetworkSlipQc.hpp), so the common-band
            // scope's read-only flag preview (SPEC_event_harness §14 rule) runs the
            // same arithmetic; its order statistics do not depend on the order in
            // which the samples arrive.
            std::vector<double> pool_dd, pool_el;
            std::vector<char>   pool_ref;
            pool_dd.reserve(samples.size());
            pool_el.reserve(samples.size());
            pool_ref.reserve(samples.size());
            for (const auto& s : samples) {
                pool_dd.push_back(s.dd);
                pool_el.push_back(s.el);
                pool_ref.push_back(s.is_ref ? char{1} : char{0});
            }
            const IfTdPairPool pstat = ifTdPoolStats(pool_dd, pool_el, pool_ref);
            const double med = pstat.med;

            // (2) Pooled elevation-normalized robust scale (zenith-equivalent m):
            //     s_pool = 1.4826·median(|z|), z = (dd−med)/ifElevMap(el).
            //     The constellation reference satellite is excluded from the scale:
            //     its dd is identically 0 by construction (grad(ref) = OMC(ref) −
            //     OMC(ref) = 0), an exact structural zero at the centre of the
            //     distribution that deflates the MAD (by 16.0 % in a 24 h, 60-station
            //     run where those zeros were 12.5 % of all samples).  The reference
            //     carries no information about the noise this threshold tracks, and
            //     the pool arm cannot flag it anyway (|dd − med| ≈ 0); the separate
            //     per-station reference gate (tier T1r) screens it.  See §6.14.5
            //     defect 2.  Fallback: if excluding leaves nothing (every sample is
            //     the reference), the full set is used so that the median is never
            //     taken over an empty set.  Reference samples are identically 0, so
            //     the pooled scale is then 0.
            const double s_pool = pstat.s_pool;
            if (static_cast<int>(samples.size()) < min_pool) ++n_sparse;

            // Group the pair's samples by station (ordered, hence deterministic) so
            // each station-pair thresholds with its own shrunk scale, then updates
            // its own EWMA once this epoch's flags are known.
            std::map<int, std::vector<const PairSample*>> by_sta;
            for (const auto& s : samples) by_sta[s.sta].push_back(&s);

            for (auto& [sta, plist] : by_sta) {
                auto& sc = stations[sta].if_pair_scale[pair];

                // Empirical-Bayes shrinkage: pool prior (cold) → station EWMA (warm).
                // Shared with the common-band scope's flag preview (§14 rule).  `w`
                // is kept only for the trace column; ifTdShrunkScale re-derives it.
                const double w = static_cast<double>(sc.count) /
                                 (static_cast<double>(sc.count) + shrink_kappa);
                const double s_eff =
                    ifTdShrunkScale(sc.count, sc.ewma_scale, s_pool, shrink_kappa);
                if (sc.count >= static_cast<long>(shrink_kappa)) ++n_warm_sp;

                std::vector<double> clean_zabs;
                for (const PairSample* sp : plist) {
                    const double emap    = ifElevMap(sp->el);
                    const double floor_m = floor_frac * sp->lambda;
                    const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                    const double abs_dev = std::abs(sp->dd - med);
                    const bool   flagged = (abs_dev > thr);
                    if (trace_flag_rates) { ++fr_if_slip.opp; if (flagged) ++fr_if_slip.flag; }  // B8/T1
                    // Per-pair run-end telemetry (§14.10 rule), knob-independent
                    // (invariant #4).
                    IfTdPairTally* tally = pair_tally ? &(*pair_tally)[pair] : nullptr;
                    if (tally) { ++tally->evaluated; if (flagged) ++tally->flagged; }
                    // The evaluation a re-seed restored (card ARL); never set with
                    // the re-seed off.
                    if (tally && sp->after_reseed) {
                        ++tally->post_flag_evaluated;
                        if (flagged) ++tally->post_flag_flagged;
                    }
                    if (flagged) {
                        // Restart only this pair's chain (§14.10 rule).  Resetting
                        // the satellite would discard the other pairs' clean
                        // gradient histories and make one pair's decision depend
                        // on another's flag, which the per-pair-kind pools exist
                        // to prevent.  Under `first` there is one chain per system
                        // and the two calls do the same thing.  Under the re-seed
                        // (card ARL) the chain keeps the flag epoch's gradient
                        // instead, with the same scoping, so the next epoch is
                        // evaluated rather than left blind.
                        IfPhaseSlipDetector& det = *stations[sp->sta].if_slip;
                        if (reseed_on_flag) {
                            const bool kept = iftd_all_pairs
                                ? det.reseedSatelliteInState(sp->state_key, sp->sat)
                                : det.reseedSatellite(sp->sat);
                            if (tally) ++(kept ? tally->reseeded : tally->reseed_nothing_kept);
                        } else if (iftd_all_pairs) {
                            det.resetSatelliteInState(sp->state_key, sp->sat);
                        } else {
                            det.resetSatellite(sp->sat);  // restart arc
                        }
                        const auto pact =
                            refloat_pair(sp->sta, sp->sat, sp->sig1, sp->sig2);
                        if (tally && pact.freed) ++tally->refloated;
                        emitEvent("T1_if_slip", "if_slip", sp->sat,
                                  stations[sp->sta].name, abs_dev, thr,
                                  pact.handled
                                      ? (pact.repaired ? "oracle_repair" : "oracle_keep")
                                      : (carry_warm ? "refloat" : "diagnostic"),
                                  (pact.handled
                                      ? (sp->sig1 + ";" + sp->sig2 + ";" + pact.detail)
                                      : (sp->sig1 + ";" + sp->sig2))
                                  // Which pair fired (§14.10 rule).  Added only
                                  // under `all`, where a satellite has several rows
                                  // per epoch and sig1;sig2 alone does not identify
                                  // the evaluation, so `first` rows keep the
                                  // pre-knob text byte for byte.
                                  + (iftd_all_pairs
                                       ? ";pair=" + sp->sig1 + "+" + sp->sig2
                                       : std::string{})
                                  // What the common-band verdict refused in this
                                  // drain (§14.12 rule).  Absent unless a verdict
                                  // restricted it, so every unscoped row keeps the
                                  // pre-knob text.
                                  + (pact.cb_kept.empty() ? std::string{}
                                       : ";cb_kept=" + pact.cb_kept)
                                  // A flag in the epoch after a re-seed (card ARL):
                                  // a slip repeated there.  Absent with the re-seed
                                  // off, so those rows keep the pre-knob text.
                                  + (sp->after_reseed ? std::string{";post_flag"}
                                                      : std::string{}));
                        // Bridge v2, flagged path: create the pend (or refresh it;
                        // refloat_pair above just voided any stale one) with the
                        // pre-slip capture and this δIF.  A station whose IF-TD
                        // refsat rebased this epoch is frame-invalid: no pend, and
                        // the arc keeps the plain free epoch.
                        // Sim-only oracle: with the level truth-repaired there is
                        // nothing to bridge, and a later replace would overwrite
                        // the repair with `pre_slip + shift`, so no pend is made.
                        if (bridge_on && !pact.handled) {
                            if (!sta_if_resets[sp->sta].empty()) {
                                ++bridge_cnt->bridge_fallback_refsat;
                            } else {
                                auto it1 = net::getArc(*bridge_arcs, {sp->sta, sp->sat, sp->sig1}, &net::ArcState::amb);
                                auto it2 = net::getArc(*bridge_arcs, {sp->sta, sp->sat, sp->sig2}, &net::ArcState::amb);
                                if (it1 == nullptr
                                    || it2 == nullptr) {
                                    ++bridge_cnt->bridge_fallback_no_carry;
                                } else {
                                    RelevelPend pd;
                                    pd.pre1 = it1->value_abs;
                                    pd.pre_var1 = it1->var;
                                    pd.pre2 = it2->value_abs;
                                    pd.pre_var2 = it2->var;
                                    pd.dif_m = sp->dd - med;
                                    pd.sigma_if_m = std::max(s_eff * emap, br_if_floor);
                                    pd.lambda_if = sp->lambda;
                                    pd.sig1 = sp->sig1;  pd.sig2 = sp->sig2;
                                    pd.tow_s = br_now_s;
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
                                    // Bridge v3 (E4): the same-pair GF-phase TD
                                    // (geometry- and clock-free) with the
                                    // station's own empirical step scale closes
                                    // the (1,2) plane at the first sweep without
                                    // waiting for the MW integer.  The raw
                                    // primary pair has to be this IF pair (true
                                    // for GPS and GAL on the triple posture); a
                                    // mismatch or a gap re-birth (no adjacent
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
                        // Warm half of §6.14.5 defect 2: the reference's structural
                        // zero never flags (|dd − med| ≈ 0 by construction), so
                        // without the is_ref guard it would always land here and
                        // deflate s_epoch, the quantity the 16.0 % deflation was
                        // measured on.  s_eff = w·ewma + (1−w)·s_pool, so both
                        // halves have to drop it or the scale stays biased once the
                        // station-pair goes warm.  Excluding it also keeps sc.count
                        // (the shrinkage weight) from counting a non-observation.
                        clean_zabs.push_back(abs_dev / emap);
                        // No delta bridge on the MW-only path (truth audit,
                        // SPEC_slip_relevel_walk §8.1 rule): an MW candidate whose
                        // IF sample did not flag is dominated by MW false alarms on
                        // an unslipped arc (of 73 applied bridges, 44 wrong and 16
                        // without truth, with deltas up to ±199 cyc at cm-level
                        // propagated σ; propagation carries measurement noise, not
                        // the risk of a wrong hypothesis).  The candidate stays
                        // unconsumed and is counted as unpaired, and its bands stay
                        // in the free set, which is the safe response.
                    }
                    // Trace row with the full decision context for this (station,
                    // sat, pair) sample.  sc.ewma_scale and sc.count are the
                    // decision-time (pre-update) values that s_eff is formed from.
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
                            << (carry_warm ? 1 : 0) << ','
                            // The detector's own reference flag (§22 column).  On a
                            // reference arc dd is identically 0, so `flagged` above
                            // is structurally false and the pooled test carries no
                            // information; the common-band scope's reference-arc
                            // clause reacts to that.  Trace-only.
                            << (sp->is_ref ? 1 : 0) << '\n';
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

        // Delta bridge: candidates whose (station, sat, pair) met no IF-TD sample
        // this epoch are counted as unpaired; their bands stay in the free set
        // (safe fallback).
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
                          (pact.handled ? (sig1 + ";" + sig2 + ";" + pact.detail)
                                        : (sig1 + ";" + sig2)));
                // Queue the satellite for a per-sat discontinuity mark at its next
                // actual emit (network.mark_refsat_reset_disc, Edge-1 piece 2;
                // refsat_mark_sats is null when off, which keeps the run
                // byte-identical).  Gated on carry_warm for the same reason as the
                // event's action field: while the carry is cold the detection is
                // diagnostic and no refloat happened, so there is no datum
                // re-reference for a consumer to react to, and a mark would report
                // an event that did not happen.
                // Sim-only oracle: an arc answered by a truth repair was not
                // re-referenced either, so it gets no mark (the same argument as
                // the carry_warm gate).  A pair where one band was still freed
                // keeps its mark.
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

std::vector<std::pair<std::string, std::string>> networkWindupRestartTargets(
        const StateManager& sm, int i, const std::string& sat,
        const net::ArcTable* arcs,
        double pl_now_s, double pl_max_gap_s, bool* via_carry,
        const ArcLedger* arc_ledger, RosterN3bCensus* n3b) {
    std::vector<std::pair<std::string, std::string>> targets;
    for (const auto& [ak, si] : sm.getAmbiguities()) {
        (void)si;
        const auto pp = StateKeys::parseAmbiguityKey(ak);
        if (pp && pp->satellite == sat) targets.emplace_back(ak, pp->signal);
    }
    bool carry = false;
    if (targets.empty() && arcs != nullptr) {
        // The map key is (station index, satellite, signal) and orders
        // lexicographically, so one lower_bound walks this station-satellite group.
        // The satellite's arcs at the station, in signal order (O8; rows without a
        // leveling carry are not carry entries).
        for (auto it = net::arcsFrom(*arcs, i, sat);
             it != arcs->end() && net::arcOfLos(it->first, i, sat); ++it) {
            if (!it->second.amb) continue;
            const double age_s = pl_now_s - it->second.amb->last_seen_s;
            if (age_s < 0.0 || age_s > pl_max_gap_s) continue;
            const std::string sig = net::signalKey(it->first.signal);
            targets.emplace_back(StateKeys::makeAmbiguityKey(sat, sig), sig);
        }
        // The arc identity from the arc ledger (card NOM step N3b): of the carry entries
        // inside the horizon (the arcs with a value), in signal order, the ledger keeps
        // its admitted records that are not dead.  Where the ledger holds no admission
        // for a carried arc, the arc's admission predates the ledger (a warm resume
        // restores the carry, not the ledger) and the carry answers, counted as the
        // site's fallback, as the latch answers for the roster reads.  The walk above is
        // the rule this replaces; every restart compares the two (census site
        // windup_restart_arcs, 0 on every census arm), and the targets are the ledger's.
        if (arc_ledger != nullptr) {
            std::vector<std::pair<std::string, std::string>> named;
            bool fallback = false;
            for (auto it = net::arcsFrom(*arcs, i, sat);
                 it != arcs->end() && net::arcOfLos(it->first, i, sat); ++it) {
                if (!it->second.amb) continue;
                const double age_s = pl_now_s - it->second.amb->last_seen_s;
                if (age_s < 0.0 || age_s > pl_max_gap_s) continue;
                const ArcRecord* a = it->second.ledger ? &*it->second.ledger : nullptr;   // one row (N4f2)
                if (a == nullptr || a->ep_first_admitted < 0) fallback = true;
                else if (a->adm_state == ArcState::Dead)       continue;
                const std::string sig = net::signalKey(it->first.signal);
                named.emplace_back(StateKeys::makeAmbiguityKey(sat, sig), sig);
            }
            if (n3b != nullptr && fallback)
                ++n3b->fallback[static_cast<std::size_t>(RosterN3bSite::WindupRestartArcs)];
            if (n3b != nullptr) {
                std::string walk, ledger;
                for (const auto& t : targets) walk += (walk.empty() ? "" : ",") + t.second;
                for (const auto& t : named) ledger += (ledger.empty() ? "" : ",") + t.second;
                signalRosterN3bCount(*n3b, RosterN3bSite::WindupRestartArcs, named == targets,
                                     fmt::format("sta {} {} walk {} ledger {}", i, sat,
                                                 walk, ledger));
            }
            targets = std::move(named);
        }
        carry = !targets.empty();
    }
    if (via_carry) *via_carry = carry;
    return targets;
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
                                   NetworkSlipOracle* oracle,
                                   const net::ArcTable* arcs,
                                   double pl_now_s, double pl_max_gap_s,
                                   const ArcLedger* arc_ledger, RosterN3bCensus* n3b) {
    if (!config_.networkCycleSlipWindupRestartRefloat()) return;
    const double now_s = static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
    const double carry_gap_s = config_.networkPhaseLevelingMaxGapS();
    int n_resp = 0;
    std::string detail;
    for (int i = i_lo; i < n_sta_total && i < static_cast<int>(stations.size()); ++i) {
        // Absent station: its model was not evaluated this epoch, so its ledger
        // holds no fresh entry.  It is left for the return epoch, where the
        // continued gap fires a fresh restart that overwrites the stale one.
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        if (!stations[i].model || !stations[i].sm) continue;
        const auto& ledger = stations[i].model->wuRestarts();
        if (ledger.empty()) continue;
        for (const auto& [sat, rec] : ledger) {
            ++qc_cnt.wu_restart_events;
            if (rec.epoch_s != now_s) {
                // A restart from an earlier epoch (the ledger survived an epoch
                // that nothing drained, e.g. the knob path raced a withheld
                // epoch).  The step it describes has been absorbed by whatever
                // happened since, and responding now would refloat a clean arc.
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
            // The arcs the restart stepped, as (ambiguity key, signal).  A restart is
            // a return after a gap longer than the wind-up horizon, so the arc has
            // usually left the model set: its ambiguity column was pruned with the
            // gap, and PASS B's pre-pass re-creates it only after this drain.  A walk
            // of the live ambiguity map alone then matches nothing while the PASS-B
            // inject folds the leveling carry across the step (card WUP section 1:
            // on the 6 h field arm all 28 restarts inside the carry horizon matched
            // no live column).  The live columns stay the target when the arc kept
            // them; otherwise the target is the (station, satellite)'s leveling carry
            // entries still inside the carry horizon, on the synthesised key the
            // PASS-B inject reads, as the gap re-entry drain does
            // (gapReentryRespondArc).  Live-column order is the map's, as before.
            bool via_carry = false;
            const std::vector<std::pair<std::string, std::string>> targets =
                networkWindupRestartTargets(sm, i, sat, arcs, pl_now_s,
                                            pl_max_gap_s, &via_carry, arc_ledger, n3b);
            // Sim-only oracle: a restart steps every signal of the satellite, so
            // each signal is offered to the oracle independently (truth localizes
            // what the band-common model step cannot).  A signal with no truth
            // jump in the window follows the false-alarm policy; signals the
            // oracle does not handle fall through to the normal refloat.
            bool orc_h = false, orc_r = false, orc_freed = false;
            std::string orc_detail;
            for (const auto& [amb_key, signal] : targets) {
                ++matched;
                if (oracle) {
                    OracleOutcome oc = OracleOutcome::NotHandled;
                    std::string det;
                    if (networkSlipOracleHandle(*oracle, i, sat, signal,
                                                now_s, RelevelEpochState::kCauseWindup,
                                                det, oc)) {
                        orc_h = true;
                        // One row per (station, satellite): the detail is that of
                        // the band the action names, the first repaired, else the
                        // first handled (see the MW site).
                        if (oc == OracleOutcome::Repaired && !orc_r) {
                            orc_r = true;
                            orc_detail = det;
                        } else if (orc_detail.empty()) {
                            orc_detail = det;
                        }
                        continue;
                    }
                }
                // The DD invariant carry owns its own funnel and is live from
                // epoch 1 (Card 4 C3c; see refloat_signal).
                if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                if (carry_warm) {
                    orc_freed = true;
                    // Only a live column takes the pending-reseed mark: a mark on an
                    // absent key would sit in the set until some later epoch's
                    // pre-pass consumed it (the gap re-entry drain's rule).  The
                    // free-epoch latch is keyed on the string and reaches the inject
                    // either way.
                    if (!via_carry) sm.markAmbiguityPendingReseed(amb_key);
                    if (relevel) {
                        relevel->free_ambs.insert({i, amb_key});
                        relevel->free_cause[{i, amb_key}] |=
                            RelevelEpochState::kCauseWindup;
                        ++qc_cnt.relevel_free_marked;
                    }
                }
            }
            // A restart is a model step, not a slip: any outstanding bridge
            // pend's pre-slip capture predates the step, and its replace would
            // re-inject a value in the pre-restart frame, so the pend is voided
            // (the rule the raw-slip drains apply on a fresh mark).
            // Sim-only oracle: when the oracle handled every signal nothing was
            // marked, so there is no stale capture to void, and voiding would
            // discard a pend the arc still needs.
            if (carry_warm && relevel_pend && (orc_freed || !orc_h))
                for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
                    if (std::get<0>(pit->first) == i && std::get<1>(pit->first) == sat) {
                        ++qc_cnt.bridge_pend_voided;
                        pit = relevel_pend->erase(pit);
                    } else ++pit;
            if (carry_warm && matched > 0) {
                ++qc_cnt.wu_restart_refloated;
                qc_cnt.wu_restart_arcs += matched;
                if (via_carry) qc_cnt.wu_restart_carry_arcs += matched;
                if (++n_resp <= 12)
                    detail += fmt::format("{}{}/{}", detail.empty() ? "" : ", ",
                                          stations[static_cast<std::size_t>(i)].name, sat);
            } else if (!carry_warm) {
                ++qc_cnt.wu_restart_cold;
            } else {
                // Warm, inside the carry horizon, and nothing to free: no live
                // column and no leveling carry entry inside the horizon (an arc
                // that never reached the carry, or one the band scope kept whole).
                // Counted apart from the responses, so a run whose restarts all
                // land here reads as unanswered rather than answered (invariant
                // #4; card WUP section 1).
                ++qc_cnt.wu_restart_no_arc;
            }
            traces.emitEvent(se.time, "T0_windup", "wu_restart", sat,
                             stations[static_cast<std::size_t>(i)].name,
                             rec.gap_s, carry_gap_s,
                             orc_h ? (orc_r ? "oracle_repair" : "oracle_keep")
                                   : (carry_warm ? (matched > 0
                                                        ? (via_carry ? "refloat_carry"
                                                                     : "refloat")
                                                        : "no_arc")
                                                 : "diagnostic"),
                             (orc_h ? fmt::format("accumulator restart; {} arc(s); {}",
                                                  matched, orc_detail)
                                    : fmt::format("accumulator restart; {} arc(s) marked",
                                                  matched)));
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

// ---------------------------------------------------------------------------
// Event harness, Study 1: the injected free_epoch arc response (SS9).
// Mirrors refloat_signal's freeing actions for a scripted (station, satellite),
// with no LocalCarry erase, no iono co-erase and no RAIM funnel queue.
// ---------------------------------------------------------------------------
void networkSlipDrainInjectedFree(const std::vector<InjectArcErase>& arc_free,
                                  std::vector<NetStation>& stations,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  bool carry_invariant, bool carry_warm,
                                  std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                  NetworkInjectState& inject,
                                  const SyncedEpoch& se, NetworkRunTraces& traces,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  RelevelEpochState* relevel,
                                  std::map<std::tuple<int, std::string, std::string>,
                                           RelevelPend>* relevel_pend) {
    if (arc_free.empty()) return;
    for (const auto& d : arc_free) {
        if (d.isErase()) continue;              // the T4 block's directive
        int matched = 0;
        bool station_seen = false;
        for (int i = 0; i < n_sta_total && i < static_cast<int>(stations.size()); ++i) {
            if (stations[static_cast<std::size_t>(i)].name != d.station) continue;
            station_seen = true;
            if (i < static_cast<int>(present.size()) && !present[i]) continue;
            StateManager& sm = *stations[static_cast<std::size_t>(i)].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                (void)sidx;
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != d.sat) continue;
                if (!d.coversSignal(parts->signal)) continue;   // band scope
                ++matched;
                // The DD invariant carry owns its own funnel and is live from
                // epoch 1 (Card 4 C3c; the same contract as refloat_signal).
                if (carry_invariant) carry_slipped_ambs.insert({ i, amb_key });
                if (carry_warm) {
                    sm.markAmbiguityPendingReseed(amb_key);
                    if (relevel) {
                        relevel->free_ambs.insert({ i, amb_key });
                        // No detector saw anything: the cause bits describe the
                        // tier that freed the arc, and here that is the harness.
                        // kCauseT4Attrib is the closest accurate tag (T4
                        // attribution is the tier whose decision is emulated) and
                        // keeps the cause classification readable.
                        relevel->free_cause[{ i, amb_key }] |=
                            RelevelEpochState::kCauseT4Attrib;
                    }
                }
            }
            // A fresh mark makes any outstanding pend's pre-slip capture stale.
            if (carry_warm && relevel_pend && matched > 0)
                for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
                    if (std::get<0>(pit->first) == i && std::get<1>(pit->first) == d.sat)
                        pit = relevel_pend->erase(pit);
                    else
                        ++pit;
        }
        if (matched > 0) {
            traces.emitEvent(se.time, "INJECT", "arc_erase", d.sat, d.station,
                             std::nullopt, std::nullopt, "injected",
                             fmt::format("detector_path;mode={};bands={};ambs={};latch={}",
                                         d.mode,
                                         [&] {
                                             if (d.signals.empty()) return std::string("all");
                                             std::string o;
                                             for (const auto& x : d.signals)
                                                 o += (o.empty() ? "" : ";") + x;
                                             return o;
                                         }(),
                                         matched,
                                         (carry_warm && relevel) ? "free_ambs"
                                                                 : "none"));
            inject.arc_erase_mode_acted[d.mode] += matched;
            inject.n_arc_erase_acted += matched;
            const std::size_t k = d.ev_index;
            if (k < inject.events.size() && !inject.onset_logged[k]) {
                ++inject.n_applied[k];
                inject.onset_logged[k] = true;
                ++inject.fired_by_kind["arc_erase"];
            }
            log->warn("Network (fault inject): TOW {:.1f} injected arc_erase "
                      "[mode={}] freed {} ambiguity(ies) of {}/{} through the "
                      "DETECTOR path - carry entry KEPT, no iono co-erase, no "
                      "funnel queue.{}",
                      se.time.tow, d.mode, matched, d.station, d.sat,
                      (carry_warm && relevel)
                          ? ""
                          : "  WARNING: the free-epoch latch is absent (carry "
                            "cold, or cycle_slip.relevel_free_epoch off) - the "
                            "mark alone does NOT deliver a free epoch.");
        } else {
            log->error("Network (fault inject): TOW {:.1f} arc_erase [mode={}] "
                       "matched no live ambiguity for {}/{} ({}).  The event will "
                       "be reported as NEVER FIRED at run-end.",
                       se.time.tow, d.mode, d.station, d.sat,
                       station_seen ? "station present, satellite not tracked"
                                    : "station not on the roster");
        }
    }
}

namespace {

/// The strongest evidence behind one gap re-entry verdict, in the priority order
/// the freed counters partition on (a single re-entry can satisfy several gates;
/// the event's `cause` field keeps all of them).
enum class GapCause { Shifted, Lli, Unmeas };

/// The cause's label, for the event detail and the per-drain log line.
const char* gapCauseName(GapCause c) {
    return c == GapCause::Shifted ? "shifted" : (c == GapCause::Lli ? "lli" : "unmeas");
}

/// The closure bucket of @ref GapReentryLedger that one reached arc landed in, so a
/// caller keeping sub-counts of the ledger (the co-resumed share) sorts its arcs
/// exactly as the responder did rather than re-deriving the rule.
enum class GapArcBucket { None, Freed, ColdSkipped, NoRelevel, DdOnly };

/// What one arc's gap re-entry response did: everything its event must state.
struct GapArcOutcome {
    bool         reached{false};     ///< A protectable carry existed (the arc entered the closure).
    bool         pl_bridged{false};  ///< …a live leveling carry entry (false ⇒ the carry_invariant vehicle).
    bool         state_live{false};  ///< The ambiguity column already existed at drain time.
    const char*  action{"none"};     ///< The event row's `action` field.
    GapArcBucket bucket{GapArcBucket::None};  ///< The ledger bucket (None when not reached); a freed arc's cause is the caller's `primary`.
};

/**
 * @brief Free one (station, satellite, signal) arc at a gap re-entry.
 *
 * The response body shared by the row-level tier (Card G) and the per-signal
 * rider (Card GRS): both answer the same question about the same carry, and a
 * single copy keeps the two tiers from drifting apart.  Counters land in @p led
 * so each tier keeps its own closed partition; the caller emits the event,
 * because only it knows the tier's identity fields.
 *
 * @param i                  App station index (already screened against i_lo).
 * @param satellite          Satellite id.
 * @param sig                The one signal this call responds on (band-scoped).
 * @param sm                 That station's state manager, mutated (pending-reseed mark).
 * @param carry_invariant    The Card-4 DD invariant carry vehicle is armed.
 * @param carry_warm         The legacy phase-leveling carry is warm.
 * @param carry_slipped_ambs C3c funnel, inserted into under @p carry_invariant.
 * @param arcs        The leveling carry (the arc table), read-only; it answers "is
 *                           there anything to protect?".
 * @param pl_now_s           This epoch's continuous GPS seconds.
 * @param pl_max_gap_s       @c network.phase_leveling_max_gap_s.
 * @param cause_bits         Free-cause bits (RelevelEpochState::free_cause).
 * @param primary            Strongest evidence (the freed buckets' partition key).
 * @param led                This tier's ledger, accumulated into.
 * @param qc_cnt             Run-end counters (only @c relevel_free_marked and
 *                           @c gap_reentry_unreachable here).
 * @param relevel            Free-epoch latch, or nullptr when relevel_free_epoch is off.
 * @param present_sigs       The phase signals of this (station, satellite) that
 *                           are present this epoch with a matching code, used
 *                           only to qualify @c gap_reentry_unreachable (Card
 *                           GRK).  Null (the default) keeps the broader count,
 *                           which is what a posture without the per-signal list
 *                           can state.
 * @param count_unreachable  Whether this call may feed @c gap_reentry_unreachable
 *                           at all (Card GRK).  True only from the row-level
 *                           tier, whose key is the tracker's band signal and can
 *                           therefore differ from the carried one.  The
 *                           per-signal tier passes false: there the returning
 *                           signal is the key, so a key mismatch cannot occur,
 *                           and a return on a signal the station does not model
 *                           would otherwise be scored against a same-band carry
 *                           that tracked through the break and has no break of
 *                           its own.
 * @return The outcome; @c reached false means the arc had no carry to protect.
 */
GapArcOutcome gapReentryRespondArc(
    int i, const std::string& satellite, const std::string& sig,
    StateManager& sm, bool carry_invariant, bool carry_warm,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const net::ArcTable& arcs,
    double pl_now_s, double pl_max_gap_s,
    std::uint8_t cause_bits, GapCause primary,
    GapReentryLedger& led, SlipQcCounters& qc_cnt, RelevelEpochState* relevel,
    const std::set<std::string>* present_sigs = nullptr,
    bool count_unreachable = true) {
    GapArcOutcome out;
    // The key is synthesized, not looked up (see the banner in
    // NetworkSlipQc.hpp).  Walking the live ambiguity map, as the MW/LLI
    // drain's refloat_signal does, cannot work for a re-entry: the arc left the
    // model set, so its ambiguity column was pruned with the gap, and PASS B's
    // measurement pre-pass (MeasurementModel::ensureRequiredStates) re-creates
    // it only after this drain, so a walk would match and free nothing.  This is
    // the key format the PASS-B inject site reads and the one the Card-G arc
    // classification synthesizes, so a freed arc reads `shifted_fresh` there by
    // construction.
    const std::string amb_key = StateKeys::makeAmbiguityKey(satellite, sig);

    // "Is there anything to protect?" is a question about the carry.  A
    // LocalCarry entry still inside the max-gap window is what the PASS-B
    // inject would fold across the break.  This is the `pl_bridged` predicate
    // the Card-G classification applies, so the instrument and the response
    // agree on the population.
    {
        const auto cit = net::getArc(arcs, std::make_tuple(i, satellite, sig), &net::ArcState::amb);
        if (cit != nullptr) {
            const double gap_s = pl_now_s - cit->last_seen_s;
            out.pl_bridged = (gap_s >= 0.0 && gap_s <= pl_max_gap_s);
        }
    }
    // Under carry_invariant the DD chord test would need
    // NetworkInvariantCarryDriver, a runNetwork local that does not
    // reach this seam (ctx carries carry_sat_vertex / carry_group_id,
    // not the driver).  So every re-entry is treated as protectable
    // there, the conservative direction: the C3c funnel insert is
    // idempotent, and an arc with no carried chord re-taps fresh, as it
    // would have anyway.  The cost is that gap_reentry_no_carry cannot
    // separate the two cases on that vehicle; the DD ledger
    // (gap_shifted_bridged / _fresh) can, from the driver.
    if (!(out.pl_bridged || carry_invariant)) {
        ++led.no_carry;
        // Why there was no carry under this key (Card GRK).  A LocalCarry entry
        // for the same (station, satellite) on another signal of the same band,
        // still inside the max-gap window, is what the PASS-B inject will fold
        // across this break, and this response cannot reach it: the tracker
        // named the band by one signal while the carry is keyed on the station's
        // canonical one.  That is the unreachable population, not an arc with
        // nothing to protect.  It is counted in both states of
        // network.cycle_slip.tracker_modeled_first, so the run end can state the
        // knob's target population either way.  The map key is (station index,
        // satellite, signal) and orders lexicographically, so one lower_bound
        // walks this station-satellite group alone.
        //
        // The carried signal must also be tracked this epoch.  Otherwise the
        // count would take arcs whose carried signal is absent from the record:
        // for a station that reports C1C, C1L, L1L, C2L, L2L, C5Q and L5Q the
        // tracker's fallback to L1L / L2L is the right choice, the carried
        // L1C / L2W arcs have no row that epoch, and their own return belongs to
        // the per-signal tier rather than being a response this tier declined.
        //
        // Only the row-level tier may score it.  In the per-signal tier the
        // returning signal is the key, so the two halves cannot name the band
        // differently; a return there on a signal the station does not model
        // (L2L where the station carries L2W) correctly finds no carry, and
        // counting it against the modelled same-band carry would score an arc
        // that tracked through this break and has no break of its own.
        const System sys = systemOfSat(satellite);
        const auto this_sig = SignalId::fromObsCode(sys, sig);
        for (auto it = (count_unreachable ? net::arcsFrom(arcs, i, satellite) : arcs.end());
             it != arcs.end() && net::arcOfLos(it->first, i, satellite); ++it) {
            if (!it->second.amb) continue;   // not a carry entry (O8)
            const std::string other = net::signalKey(it->first.signal);
            const auto other_sig = SignalId::fromObsCode(sys, other);
            if (other == sig || !other_sig || !this_sig) continue;
            if (other_sig->digit != this_sig->digit) continue;   // a different band
            if (present_sigs != nullptr && present_sigs->count(other) == 0) continue;
            const double gap_s = pl_now_s - it->second.amb->last_seen_s;
            if (gap_s >= 0.0 && gap_s <= pl_max_gap_s) {
                ++qc_cnt.gap_reentry_unreachable;              // once per arc
                break;
            }
        }
        return out;
    }
    ++led.arcs;
    out.reached = true;

    // Whether the state exists yet is counted, not assumed; usually it does
    // not (the column was pruned with the gap, see above).  The free-epoch
    // latch is keyed on the string and survives to the inject either way.
    // Only the pending-reseed mark needs a live state, because a mark on an
    // absent key would sit in the set until some later epoch's pre-pass
    // consumed it, a response acting on an old event.
    out.state_live = sm.getAmbiguities().count(amb_key) != 0;
    if (!out.state_live) ++led.state_absent;

    // The DD invariant carry owns its own funnel and is live from
    // epoch 1 (Card 4 C3c; the same contract as refloat_signal).
    if (carry_invariant) {
        carry_slipped_ambs.insert({i, amb_key});
        ++led.dd_marked;
    }
    if (carry_warm) {              // cold: nothing stale to refloat (counted below)
        if (out.state_live) sm.markAmbiguityPendingReseed(amb_key);
        if (relevel) {
            relevel->free_ambs.insert({i, amb_key});
            relevel->free_cause[{i, amb_key}] |= cause_bits;
            ++qc_cnt.relevel_free_marked;
        }
    }
    if (!carry_warm) {
        ++led.cold_skipped;
        out.bucket = GapArcBucket::ColdSkipped;
    } else if (!relevel && carry_invariant) {
        // Under carry_invariant the leveling local carry is superseded and
        // structurally empty (which is why the loader disarms
        // relevel_free_epoch there), so the C3c funnel insert above is the
        // whole response and nothing is missing.  The bucket is separate
        // from `no_relevel`: this one is by design, the other is a response
        // that could not land.
        ++led.dd_only;
        out.bucket = GapArcBucket::DdOnly;
    } else if (!relevel) {
        // The mark alone does not deliver the free epoch (PASS B consumes
        // it before the inject), so this bucket records that instead of
        // reporting a response that cannot land.
        ++led.no_relevel;
        out.bucket = GapArcBucket::NoRelevel;
    } else {
        if (primary == GapCause::Shifted)  ++led.freed_shifted;
        else if (primary == GapCause::Lli) ++led.freed_lli;
        else                               ++led.freed_unmeas;
        out.bucket = GapArcBucket::Freed;
    }
    out.action = (carry_warm && relevel)
                     ? "refloat"
                     : (!carry_warm ? "cold"
                                    : (carry_invariant ? "dd_only" : "no_relevel"));
    return out;
}

/// This epoch's raw row for (station @p i, @p satellite), or null (Card GRK).
///
/// The row carries the per-signal list that @c gap_reentry_unreachable qualifies
/// on.  A linear scan suffices: a station's row vector holds one entry per
/// tracked satellite (tens), and it is walked once per resumption.
const RawSlipObs* gapReentryFindRow(const RawSlipEpoch& raw, int i,
                                    const std::string& satellite) {
    if (i < 0 || i >= static_cast<int>(raw.rows.size())) return nullptr;
    for (const auto& r : raw.rows[static_cast<std::size_t>(i)])
        if (r.satellite == satellite) return &r;
    return nullptr;
}

/// The phase signals of @p row that are present with a matching code, or an
/// empty set when the row carries no per-signal list (the Card GRS tier
/// unarmed).  An empty set is passed to the responder as a null pointer, which
/// keeps the broader unreachable count instead of reporting a zero.
std::set<std::string> gapReentryPresentSignals(const RawSlipObs* row) {
    std::set<std::string> out;
    if (row != nullptr)
        for (const auto& s : row->sigs) out.insert(s.sig);
    return out;
}

/// Void any outstanding bridge pend on (station, satellite): a fresh mark makes
/// its pre-slip capture stale, so a later replace would re-inject a level from
/// before the break.  The raw-slip drains apply the same rule.
void gapReentryVoidPends(int i, const std::string& satellite,
                         std::map<std::tuple<int, std::string, std::string>,
                                  RelevelPend>* relevel_pend,
                         SlipQcCounters& qc_cnt) {
    if (!relevel_pend) return;
    for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
        if (std::get<0>(pit->first) == i && std::get<1>(pit->first) == satellite) {
            ++qc_cnt.bridge_pend_voided;
            pit = relevel_pend->erase(pit);
        } else ++pit;
}

// ---------------------------------------------------------------------------
// Shared lookup helpers (Card SID phase 3)
// ---------------------------------------------------------------------------

/// Record one arc's response outcome for the unified census (R4).  Null sink ⇒
/// no roster was set up (a context no network run built, such as a unit test's;
/// every network run builds it since card NOM step N3a) and nothing is recorded.
void sidRecordOutcome(ArcReentryOutcomeMap* sink, int i, const std::string& sat,
                      const std::string& sig, ArcReentryOutcome o) {
    if (sink == nullptr) return;
    (*sink)[ArcLedger::Key{ i, sat, sig }] = o;
}

/// (station, satellite) -> this epoch's row-level resumption, for the per-arc
/// verdict lookup.  A satellite produces at most one row resumption per epoch
/// (the raw pass keys its history on the satellite), so the map is injective.
std::map<std::pair<int, std::string>, const GapResumption*>
sidIndexRowResumptions(const RawSlipEpoch& raw) {
    std::map<std::pair<int, std::string>, const GapResumption*> out;
    for (const auto& g : raw.resumptions)
        out.emplace(std::make_pair(g.station, g.satellite), &g);
    return out;
}

/// (station, satellite, signal) -> this epoch's per-signal resumption.
std::map<std::tuple<int, std::string, std::string>, const SigResumption*>
sidIndexSigResumptions(const RawSlipEpoch& raw) {
    std::map<std::tuple<int, std::string, std::string>, const SigResumption*> out;
    for (const auto& g : raw.sig_resumptions)
        out.emplace(std::make_tuple(g.station, g.satellite, g.signal), &g);
    return out;
}

/// The row-level tier's verdict for one arc of that satellite's return.
///
/// The LLI read is band-scoped: the row carries one flag per band of its primary
/// pair, so the arc's own signal picks the flag.  A modelled signal that is not
/// in the primary pair (a third band) gets no LLI of its own rather than the
/// other band's, which would attribute a receiver verdict to the wrong signal.
struct SidVerdict {
    bool has{false};        ///< a raw tier saw this arc's return this epoch.
    bool clean{true};       ///< it saw it and called it clean (bridge by design).
    bool shifted{false};
    bool lli{false};
    bool unmeas{false};
    bool from_signal{false};///< the verdict came from the per-signal tier.
    bool shared{false};     ///< taken from a co-resumed sibling (the co-resumed share).
    GapCause primary{GapCause::Unmeas};
    std::string cause_txt;
};

SidVerdict sidRowVerdict(const GapResumption& g, const std::string& sig,
                         const SlipQcParams& qc) {
    SidVerdict v;
    v.has     = true;
    const int lli_of_sig = (sig == g.sig1) ? g.lli1 : ((sig == g.sig2) ? g.lli2 : 0);
    v.shifted = g.shifted;
    v.lli     = qc.gap_reentry_lli && lli_of_sig != 0;
    v.unmeas  = qc.gap_reentry_unmeasurable && !g.measurable;
    v.clean   = !(v.shifted || v.lli || v.unmeas);
    v.primary = v.shifted ? GapCause::Shifted : (v.lli ? GapCause::Lli : GapCause::Unmeas);
    if (v.shifted) v.cause_txt = "shifted";
    if (v.lli)     v.cause_txt += v.cause_txt.empty() ? "lli" : "|lli";
    if (v.unmeas)  v.cause_txt += v.cause_txt.empty() ? "unmeas" : "|unmeas";
    return v;
}

SidVerdict sidSigVerdict(const SigResumption& g, const SlipQcParams& qc) {
    SidVerdict v;
    v.has         = true;
    v.from_signal = true;
    v.shifted     = g.shifted;
    v.lli         = qc.gap_reentry_lli && g.lli != 0;
    v.unmeas      = qc.gap_reentry_per_signal_unmeasurable && !g.measurable;
    v.clean       = !(v.shifted || v.lli || v.unmeas);
    v.primary = v.shifted ? GapCause::Shifted : (v.lli ? GapCause::Lli : GapCause::Unmeas);
    if (v.shifted) v.cause_txt = "shifted";
    if (v.lli)     v.cause_txt += v.cause_txt.empty() ? "lli" : "|lli";
    if (v.unmeas)  v.cause_txt += v.cause_txt.empty() ? "unmeas" : "|unmeas";
    return v;
}

// ---------------------------------------------------------------------------
// The co-resumed share (network.cycle_slip.gap_reentry_coresumed_share)
// ---------------------------------------------------------------------------
//
// The per-signal tier judges each returning signal by its geometry-free level
// against a partner that tracked through the gap, and that test cannot see a step
// common to the candidate and its partner.  The signals of one (station,
// satellite) that return at the same epoch are one re-acquisition of that line of
// sight, so the share lets a verdict on one of them stand for all of them.  Only
// co-resumed signals are grouped: a signal that tracked through has no resumption
// and is never in a group.
//
// The trigger may be any signal of the list, which holds every phase with a
// matching code (NetworkRawObservables.cpp), modelled or not, and an LLI or
// unmeasurable verdict triggers like a level shift.  That is wider than the
// measured case (a shifted, modelled trigger); the ledger counts the lines of
// sight whose trigger had no leveling carry, and splits the frees by the trigger's
// cause, so a field arm can see which part of the scope acted.

/// The line of sight of station @p i and @p satellite, or nullopt when the satellite
/// id does not parse (such a resumption is not grouped: it never triggers and is
/// never shared).
std::optional<LosId> coresumedLos(int i, const std::string& satellite) {
    const std::optional<SatId> sat = SatId::parse(satellite);
    if (!sat) return std::nullopt;
    return LosId{ StationId{ i }, *sat };
}

/// One line of sight's per-signal resumptions at this epoch, and the first of them
/// (in list order) with a verdict of its own.
struct CoresumedGroup {
    int                  n_signals{0};    ///< co-resumed signals of this (station, satellite).
    const SigResumption* trigger{nullptr};///< first with its own verdict; null when all are clean.
    SidVerdict           trigger_v;       ///< that verdict (not clean when @ref trigger is set).
};

/// Line of sight -> its co-resumed group.
using CoresumedGroups = std::map<LosId, CoresumedGroup>;

/// Group this epoch's per-signal resumptions by line of sight.  The verdict is
/// @ref sidSigVerdict, the per-signal tier's own ladder, so a trigger is exactly a
/// resumption the tier would answer for itself.  The list is built station by
/// station and, within a row, in the row's signal order, so the trigger choice is
/// deterministic.
CoresumedGroups coresumedGroups(const RawSlipEpoch& raw, const SlipQcParams& qc) {
    CoresumedGroups out;
    for (const auto& g : raw.sig_resumptions) {
        const std::optional<LosId> los = coresumedLos(g.station, g.satellite);
        if (!los) continue;
        CoresumedGroup& grp = out[*los];
        ++grp.n_signals;
        if (grp.trigger != nullptr) continue;
        const SidVerdict v = sidSigVerdict(g, qc);
        if (!v.clean) {
            grp.trigger   = &g;
            grp.trigger_v = v;
        }
    }
    return out;
}

/// The group whose verdict a clean co-resumed signal of (@p i, @p satellite)
/// takes, or null when no co-resumed signal of that line of sight was judged (or
/// the share is not active, which leaves @p groups empty).  A clean signal is never
/// its own group's trigger, so a non-null group always names a sibling.
const CoresumedGroup* coresumedShareFor(const CoresumedGroups& groups, int i,
                                        const std::string& satellite) {
    if (groups.empty()) return nullptr;
    const std::optional<LosId> los = coresumedLos(i, satellite);
    if (!los) return nullptr;
    const auto it = groups.find(*los);
    if (it == groups.end() || it->second.trigger == nullptr) return nullptr;
    return &it->second;
}

/// True when an earlier drain of this epoch (the MW, LLI, IF-TD or wind-up drain)
/// already put the ambiguity of (@p i, @p satellite, @p sig) in the free set.  The
/// share then has nothing to add: the set is idempotent, and a second response
/// would only count the arc twice and trace it twice.  Always false without the
/// free-epoch latch (relevel_free_epoch off), where there is no such set to read.
bool gapReentryAlreadyFreed(const RelevelEpochState* relevel, int i,
                            const std::string& satellite, const std::string& sig) {
    return relevel != nullptr
        && relevel->free_ambs.count({ i, StateKeys::makeAmbiguityKey(satellite, sig) }) != 0;
}

/// The `pl_bridged` predicate of the responder for one (station, satellite,
/// signal): a leveling carry entry last seen inside the max-gap window.  Used by
/// the share to state whether its trigger was a carried signal.
bool gapReentryCarryProtectable(
    const net::ArcTable& arcs,
    int i, const std::string& satellite, const std::string& sig,
    double pl_now_s, double pl_max_gap_s) {
    const auto cit = net::getArc(arcs, std::make_tuple(i, satellite, sig), &net::ArcState::amb);
    if (cit == nullptr) return false;
    const double gap_s = pl_now_s - cit->last_seen_s;
    return gap_s >= 0.0 && gap_s <= pl_max_gap_s;
}

/// Sort one shared signal's response into the share's ledger, bucket for bucket as
/// the responder sorted it into the tier's (freed split by the trigger's cause).
void coresumedShareTally(CoresumedShareLedger& sh, const GapArcOutcome& out,
                         GapCause trigger_primary) {
    if (!out.reached) { ++sh.no_carry; return; }
    switch (out.bucket) {
    case GapArcBucket::Freed:
        if (trigger_primary == GapCause::Shifted)  ++sh.freed_shifted;
        else if (trigger_primary == GapCause::Lli) ++sh.freed_lli;
        else                                       ++sh.freed_unmeas;
        break;
    case GapArcBucket::ColdSkipped: ++sh.cold_skipped; break;
    case GapArcBucket::NoRelevel:   ++sh.no_relevel;   break;
    case GapArcBucket::DdOnly:      ++sh.dd_only;      break;
    case GapArcBucket::None:        break;   // a reached arc always has a bucket
    }
}

/// The verdict a clean co-resumed signal takes from its group: the trigger's
/// primary cause, so the freed buckets keep their partition, under the cause text
/// `shared`.
SidVerdict sidSharedVerdict(const CoresumedGroup& grp) {
    SidVerdict v;
    v.has         = true;
    v.clean       = false;
    v.from_signal = true;
    v.shared      = true;
    v.primary     = grp.trigger_v.primary;
    v.cause_txt   = "shared";
    return v;
}

}  // namespace

void networkSlipDrainGapReentry(std::vector<NetStation>& stations,
                                const std::vector<bool>& present,
                                int i_lo, int n_sta_total,
                                bool carry_invariant, bool carry_warm,
                                std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                const RawSlipEpoch& raw, const SlipQcParams& qc,
                                const net::ArcTable& arcs,
                                double pl_now_s, double pl_max_gap_s,
                                SlipQcCounters& qc_cnt,
                                const SyncedEpoch& se, NetworkRunTraces& traces,
                                const std::shared_ptr<spdlog::logger>& log,
                                RelevelEpochState* relevel,
                                std::map<std::tuple<int, std::string, std::string>,
                                         RelevelPend>* relevel_pend,
                                const ArcReentryEpoch* sid,
                                ArcReentryOutcomeMap* sid_out,
                                ArcLedger* arc_ledger) {
    if (!qc.gap_reentry) return;
    // With the ledger armed (Card SID phase 3), the population is the ledger's and
    // the raw list is only the verdict source, so an epoch with no raw resumption
    // still has work.
    const bool ledger_driven = (sid != nullptr && sid->armed);
    if (raw.resumptions.empty() && !ledger_driven) return;
    int n_resp = 0;
    std::string detail_log;
    // This tier's counters are accumulated here and folded into the historical
    // flat fields at the end (the run-end Card-G line and the unit suite name
    // them individually); the shared responder speaks the ledger shape.
    GapReentryLedger led;
    // The resumption list is built station-ordered by the qualify pass, and this
    // walk touches only station i's own StateManager plus the two epoch-scoped
    // sets, serially in that order, so it is deterministic.
    for (const auto& g : raw.resumptions) {
        const int i = g.station;
        if (i < 0 || i >= n_sta_total || i >= static_cast<int>(stations.size())) continue;
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        // --- the verdict, in the cause priority the counters are split on ----
        // A single re-entry can satisfy several gates (in field data a re-entry
        // can be shifted and LLI-flagged on both bands); the primary cause is the
        // strongest evidence, so the three freed counters partition the freed
        // population exactly while the event's `cause` field keeps all of them.
        const bool lli_set  = qc.gap_reentry_lli && (g.lli1 != 0 || g.lli2 != 0);
        const bool unmeas   = qc.gap_reentry_unmeasurable && !g.measurable;
        if (!g.shifted && !lli_set && !unmeas) {
            // A clean re-entry is bridged by design.  It is recorded so the
            // unified census (R4) can tell "bridged because a tier looked and
            // found nothing" from "bridged because nothing looked".
            for (const std::string& s : { g.sig1, g.sig2 })
                if (!s.empty())
                    sidRecordOutcome(sid_out, i, g.satellite, s,
                                     ArcReentryOutcome::BridgedVerdict);
            continue;
        }
        ++led.verdicts;
        // The legacy pivot's ambiguities are gauge-pinned: refloating one would
        // move the frame that every emitted phase bias references, which is the
        // datum-slip re-reference path (MW drain), not this one.  Skipped and
        // counted (pivot_skipped).
        if (i < i_lo) { ++led.pivot_skipped; continue; }
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
        const GapCause primary = g.shifted ? GapCause::Shifted
                                           : (lli_set ? GapCause::Lli : GapCause::Unmeas);
        // With the ledger armed (Card SID phase 3), this tier's detections,
        // verdicts and traces are everything above, unchanged so an A/B can still
        // read what the raw tier saw, and the response moves to the ledger loop
        // below.
        if (ledger_driven) continue;
        StateManager& sm = *stations[static_cast<std::size_t>(i)].sm;
        bool voided_pends = false;
        // The signals this (station, satellite) tracks this epoch, which qualify
        // the unreachable count (Card GRK).  Built once per resumption and shared
        // by both bands.
        const std::set<std::string> present_sigs =
            gapReentryPresentSignals(gapReentryFindRow(raw, i, g.satellite));
        const std::set<std::string>* present_ptr =
            present_sigs.empty() ? nullptr : &present_sigs;
        // --- Extra channels of the same two bands (Card SID phase 6b, R3) --------
        // This tier keys on the satellite and frees the tracker's one phase per band.
        // That is the whole modelled set on a one-channel-per-band posture, but not
        // under `processing.channel_mode: all`: there the station also carries every
        // other admitted phase attribute of those bands, each an ordinary float arc
        // with its own carried level, and a whole-row gap takes all of them down
        // together.  The per-signal tier cannot pick them up either, because it
        // skips by construction every satellite this tier already owns
        // (`row_resumed`); without this block the extra arcs would fold their level
        // across the break with no response (standing invariant #1).
        //
        // The set is the station's republished second-phase cells (the selection's
        // own output), restricted to this satellite and to the two bands that broke
        // (band scoping, as in the rest of the tier).  It is empty on every posture
        // with no extra family, so the loop below then runs over exactly
        // `{sig1, sig2}` and the behaviour is unchanged.
        std::vector<std::string> sigs6b{ g.sig1, g.sig2 };
        {
            const NetStation& stx = stations[static_cast<std::size_t>(i)];
            const System sys = systemOfSat(g.satellite);
            const auto band1 = SignalId::fromObsCode(sys, g.sig1);   // the two bands that broke
            const auto band2 = SignalId::fromObsCode(sys, g.sig2);
            for (const std::string& cell : stx.spc_cells) {
                const auto parts = StateKeys::parseSatPhaseBiasKey(cell);
                if (!parts || parts->satellite != g.satellite) continue;
                const std::string& s = parts->signal;
                const auto cell_sig = SignalId::fromObsCode(sys, s);
                if (!cell_sig) continue;
                const bool on_band = (band1 && cell_sig->digit == band1->digit)
                                  || (band2 && cell_sig->digit == band2->digit);
                if (!on_band) continue;
                if (std::find(sigs6b.begin(), sigs6b.end(), s) != sigs6b.end()) continue;
                sigs6b.push_back(s);
                ++qc_cnt.gap_reentry_extra_channel;
            }
        }
        for (const std::string& sig : sigs6b) {
            if (sig.empty()) continue;
            const GapArcOutcome out =
                gapReentryRespondArc(i, g.satellite, sig, sm, carry_invariant,
                                     carry_warm, carry_slipped_ambs, arcs,
                                     pl_now_s, pl_max_gap_s, cause_bits, primary,
                                     led, qc_cnt, relevel, present_ptr);
            sidRecordOutcome(sid_out, i, g.satellite, sig,
                             out.reached ? ArcReentryOutcome::Freed
                                         : ArcReentryOutcome::NoCarry);
            if (!out.reached) continue;   // no carry to protect: the inject would not bridge it
            // One event per arc reached (invariant #7's re-coupling edge needs
            // an identity, not only a count).  `statistic` is the raw ΔMW in
            // metres when the break was measurable and blank otherwise: a 0.0
            // would read as "the level did not move", an absence-of-evidence
            // claim that Card G is designed not to make.
            traces.emitEvent(se.time, "T0_gap", "reentry_relevel", g.satellite,
                             stations[static_cast<std::size_t>(i)].name,
                             g.measurable ? std::optional<double>(g.d_mw) : std::nullopt,
                             std::nullopt, out.action,
                             fmt::format("{};gap_ep={};dGF={};lli1={};lli2={};cause={};"
                                         "carry={};state={}",
                                         sig, g.gap_epochs,
                                         g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                      : std::string{},
                                         g.lli1, g.lli2, cause_txt,
                                         // Why this arc qualified as protectable, and
                                         // whether the column existed yet, recorded
                                         // per event.
                                         out.pl_bridged ? "pl" : "dd",
                                         out.state_live ? "live" : "absent"));
            // A fresh mark on this (station, sat) makes any outstanding bridge
            // pend's pre-slip capture stale; the raw-slip drains apply the same
            // rule for the same reason (its replace would re-inject a level from
            // before the break).  Once per resumption, not per band.
            if (carry_warm && !voided_pends) {
                voided_pends = true;
                gapReentryVoidPends(i, g.satellite, relevel_pend, qc_cnt);
            }
            if (++n_resp <= 12)
                detail_log += fmt::format("{}{}/{}/{} [{}]",
                                          detail_log.empty() ? "" : ", ",
                                          stations[static_cast<std::size_t>(i)].name,
                                          g.satellite, sig, gapCauseName(primary));
        }
    }

    // ----------------------------------------------------------------------
    // The response over the ledger's admitted population (Card SID phase 3)
    // ----------------------------------------------------------------------
    //
    // This loop owns the census of the whole population, including the arcs it
    // hands to the per-signal tier, so `ledger_reentry_arcs` is a denominator
    // counted exactly once, and the four dispositions below (freed, bridged,
    // no_carry, pivot_skipped) partition it.  The response itself is the shared
    // responder the raw tiers call, keyed by the same (station, satellite,
    // signal) as the ledger arc, so a key mismatch cannot occur and
    // `gap_reentry_unreachable` is not fed from here.
    if (ledger_driven) {
        qc_cnt.ledger_reentry_outside_horizon += sid->outside_horizon;
        qc_cnt.ledger_reentry_not_modelled    += sid->not_modelled;
        const auto row_index = sidIndexRowResumptions(raw);
        const auto sig_index = sidIndexSigResumptions(raw);
        // The co-resumed share, grouped only when it is active, so the default path
        // looks nothing new up.
        CoresumedGroups share_groups;
        if (qc.coresumedShareActive()) share_groups = coresumedGroups(raw, qc);
        for (const ArcReentryArc& a : sid->arcs) {
            const int i = a.station;
            if (i < 0 || i >= n_sta_total || i >= static_cast<int>(stations.size())) continue;
            if (i < static_cast<int>(present.size()) && !present[i]) continue;
            ++qc_cnt.ledger_reentry_arcs;
            if (a.state_recreated) ++qc_cnt.ledger_reentry_state_recreated;

            // Verdict lookup, keyed by the arc identity.  The per-signal tier's
            // verdict wins where it exists: its key is the returning signal, so it
            // is the sharper statement, and the raw pass already de-duplicates the
            // two populations by satellite.
            const auto sit = sig_index.find(std::make_tuple(i, a.satellite, a.signal));
            const auto rit = row_index.find(std::make_pair(i, a.satellite));
            SidVerdict v;
            if (sit != sig_index.end())      v = sidSigVerdict(*sit->second, qc);
            else if (rit != row_index.end()) v = sidRowVerdict(*rit->second, a.signal, qc);
            // The co-resumed share: a clean per-signal verdict whose line of sight
            // has a judged co-resumed sibling takes that sibling's verdict.  The arc
            // is then routed to the per-signal tier below, which frees it, rather
            // than being counted bridged here.  An arc an earlier drain already
            // freed keeps its clean verdict, as the per-signal tier decides the same
            // way (gapReentryAlreadyFreed; nothing between the two drains frees it).
            if (v.has && v.from_signal && v.clean) {
                const CoresumedGroup* grp = coresumedShareFor(share_groups, i, a.satellite);
                if (grp != nullptr
                    && !gapReentryAlreadyFreed(relevel, i, a.satellite, a.signal))
                    v = sidSharedVerdict(*grp);
            }

            if (v.has) {
                ++qc_cnt.ledger_reentry_with_verdict;
                if (v.from_signal) ++qc_cnt.ledger_reentry_sig_tier;
                else               ++qc_cnt.ledger_reentry_row_tier;
                if (v.clean) {
                    ++qc_cnt.ledger_reentry_v_clean;
                    ++qc_cnt.ledger_reentry_bridged;
                    sidRecordOutcome(sid_out, i, a.satellite, a.signal,
                                     ArcReentryOutcome::BridgedVerdict);
                    continue;
                }
                if (v.primary == GapCause::Shifted)   ++qc_cnt.ledger_reentry_v_shifted;
                else if (v.primary == GapCause::Lli)  ++qc_cnt.ledger_reentry_v_lli;
                else                                  ++qc_cnt.ledger_reentry_v_unmeas;
                if (v.shared) ++qc_cnt.ledger_reentry_v_shared;
            } else {
                ++qc_cnt.ledger_reentry_no_verdict;
                // Attribute the no-verdict return (Card SID phase 4a, S1).  This
                // population is a screen-side decoupling: the modelled row of a
                // signal disappears while `raw_observed` keeps it and the satellite
                // stays modelled, so neither raw tier sees a break.  The reason mask
                // stamped on the admitted gap that preceded this return says which
                // screen removed the row.  It is read off the arc, not off the
                // step: the mask was written when the gap opened and survives it
                // unchanged.
                if (arc_ledger != nullptr) {
                    if (qc_cnt.ledger_reentry_nv_reason.empty())
                        qc_cnt.ledger_reentry_nv_reason.assign(kArcReasonCount, 0);
                    const ArcRecord* ar = arc_ledger->find(
                        ArcLedger::Key{ i, a.satellite, a.signal });
                    const unsigned m = ar ? ar->adm_reason_mask : kArcReasonNone;
                    if (m == kArcReasonNone) ++qc_cnt.ledger_reentry_nv_no_mask;
                    for (int b = 0; b < kArcReasonCount; ++b)
                        if (m & (1u << b))
                            ++qc_cnt.ledger_reentry_nv_reason[static_cast<std::size_t>(b)];
                }
            }

            // The gauge-pinned legacy pivot: its ambiguities are pinned, so a
            // local refloat has no meaning there, and the arc is counted instead.
            // Tested before the tier routing so the four dispositions partition
            // the population exactly: arcs = freed + bridged + no_carry +
            // pivot_skipped, whichever tier made the response.
            if (i < i_lo) { ++qc_cnt.ledger_reentry_pivot_skipped; continue; }
            // The per-signal tier owns the response for its own arcs, exactly as
            // it does unarmed: its band scoping, its counters, its event.
            if (v.has && v.from_signal) continue;

            // No-verdict policy (R2): `bridge` is the default behaviour, made
            // visible; `refloat` routes the arc down the unmeasurable path, which
            // network.cycle_slip.gap_reentry_relevel_unmeasurable then decides.
            // With that sub-knob off, `refloat` is counted and inert; the
            // loader's startup line states the dependency.
            const bool respond = v.has || (sid->refloat_no_verdict
                                           && qc.gap_reentry_unmeasurable);
            const char* nv_action = "bridge";
            GapArcOutcome out;
            if (respond) {
                const GapCause primary = v.has ? v.primary : GapCause::Unmeas;
                out = gapReentryRespondArc(i, a.satellite, a.signal,
                                           *stations[static_cast<std::size_t>(i)].sm,
                                           carry_invariant, carry_warm,
                                           carry_slipped_ambs, arcs,
                                           pl_now_s, pl_max_gap_s,
                                           RelevelEpochState::kCauseGap, primary,
                                           led, qc_cnt, relevel,
                                           /*present_sigs=*/nullptr,
                                           /*count_unreachable=*/false);
                nv_action = out.action;
                if (out.reached) ++qc_cnt.ledger_reentry_freed;
                else             ++qc_cnt.ledger_reentry_no_carry;
                sidRecordOutcome(sid_out, i, a.satellite, a.signal,
                                 out.reached ? ArcReentryOutcome::Freed
                                             : ArcReentryOutcome::NoCarry);
            } else {
                ++qc_cnt.ledger_reentry_bridged;
                nv_action = sid->refloat_no_verdict ? "declined" : "bridge";
                sidRecordOutcome(sid_out, i, a.satellite, a.signal,
                                 ArcReentryOutcome::BridgedNoVerdict);
            }

            const std::string& sta_name = stations[static_cast<std::size_t>(i)].name;
            if (!v.has) {
                // The edge invariant #7 asks for, on the no-verdict half: every
                // arc of this population gets an identity, a gap length and an
                // action, in both policies.  Throttled by the ledger's own per
                // (station, transition) holdoff so the stream stays bounded at
                // 305 stations.
                if (arc_ledger == nullptr
                    || arc_ledger->eventAllowed(i, ArcTransition::Reentry)) {
                    ++qc_cnt.ledger_reentry_events;
                    traces.emitEvent(se.time, "ARC", "arc_reentry", a.satellite,
                                     sta_name, static_cast<double>(a.gap_ep),
                                     std::nullopt, nv_action,
                                     fmt::format("signal={};verdict=none;src=ledger;"
                                                 "gap_ep={};gap_s={:.0f};policy={};"
                                                 "state_recreated={};carry={}",
                                                 a.signal, a.gap_ep, a.gap_s,
                                                 sid->refloat_no_verdict ? "refloat"
                                                                         : "bridge",
                                                 a.state_recreated ? 1 : 0,
                                                 out.pl_bridged ? "pl" : "dd"));
                }
            }
            if (!out.reached) continue;
            // The re-coupling edge, in the tier's own vocabulary so the existing
            // refloat instruments count it; `src=ledger` separates these rows from
            // the raw-driven ones without re-deriving the population.
            traces.emitEvent(se.time, "T0_gap", "reentry_relevel", a.satellite,
                             sta_name, std::nullopt, std::nullopt, out.action,
                             fmt::format("{};src=ledger;gap_ep={};cause={};carry={};"
                                         "state={};tier={};state_recreated={}",
                                         a.signal, a.gap_ep,
                                         v.has ? v.cause_txt : std::string("none"),
                                         out.pl_bridged ? "pl" : "dd",
                                         out.state_live ? "live" : "absent",
                                         v.has ? "row" : "no_verdict",
                                         a.state_recreated ? 1 : 0));
            if (carry_warm)
                gapReentryVoidPends(i, a.satellite, relevel_pend, qc_cnt);
            if (++n_resp <= 12)
                detail_log += fmt::format("{}{}/{}/{} [{}]",
                                          detail_log.empty() ? "" : ", ",
                                          sta_name, a.satellite, a.signal,
                                          v.has ? gapCauseName(v.primary) : "no_verdict");
        }
    }
    // Fold this tier's ledger into the historical flat fields.  They are the
    // names the run-end Card-G line and the unit suite read, so the ledger is
    // an internal shape only: no counter changes meaning here.
    qc_cnt.gap_reentry_verdicts      += led.verdicts;
    qc_cnt.gap_reentry_pivot_skipped += led.pivot_skipped;
    qc_cnt.gap_reentry_arcs          += led.arcs;
    qc_cnt.gap_reentry_no_carry      += led.no_carry;
    qc_cnt.gap_reentry_state_absent  += led.state_absent;
    qc_cnt.gap_reentry_freed_shifted += led.freed_shifted;
    qc_cnt.gap_reentry_freed_lli     += led.freed_lli;
    qc_cnt.gap_reentry_freed_unmeas  += led.freed_unmeas;
    qc_cnt.gap_reentry_cold_skipped  += led.cold_skipped;
    qc_cnt.gap_reentry_no_relevel    += led.no_relevel;
    qc_cnt.gap_reentry_dd_only       += led.dd_only;
    qc_cnt.gap_reentry_dd_marked     += led.dd_marked;
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

// ---------------------------------------------------------------------------
// Per-signal gap re-entry response (Card GRS)
// ---------------------------------------------------------------------------
void networkSlipDrainGapReentryPerSignal(
    std::vector<NetStation>& stations, const std::vector<bool>& present,
    int i_lo, int n_sta_total, bool carry_invariant, bool carry_warm,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const RawSlipEpoch& raw, const SlipQcParams& qc,
    const net::ArcTable& arcs,
    double pl_now_s, double pl_max_gap_s, SlipQcCounters& qc_cnt,
    const SyncedEpoch& se, NetworkRunTraces& traces,
    const std::shared_ptr<spdlog::logger>& log, RelevelEpochState* relevel,
    std::map<std::tuple<int, std::string, std::string>, RelevelPend>* relevel_pend,
    const ArcReentryEpoch* sid, ArcReentryOutcomeMap* sid_out) {
    if (!qc.gap_reentry || !qc.gap_reentry_per_signal) return;
    if (raw.sig_resumptions.empty()) return;
    // The decoupling-edge trace below is the same in both ledger states (Card SID
    // phase 3).  With the ledger armed, the response runs over the ledger arcs
    // that carry one of this tier's verdicts, and the row-level drain owns the
    // census.
    const bool ledger_driven = (sid != nullptr && sid->armed);
    std::set<std::tuple<int, std::string, std::string>> sid_pop;
    if (ledger_driven)
        for (const ArcReentryArc& a : sid->arcs)
            sid_pop.emplace(a.station, a.satellite, a.signal);
    int n_resp = 0;
    std::string detail_log;
    GapReentryLedger& led = qc_cnt.gap_sig;   // this tier owns its own ledger
    // The co-resumed share (network.cycle_slip.gap_reentry_coresumed_share): this
    // epoch's resumptions grouped by line of sight, each group with its first
    // judged signal as the trigger.  Grouped only when the share is active, so the
    // default path reads and counts nothing new.  The denominator counts only the
    // groups at a station this walk answers (in range, present, not gauge-pinned).
    CoresumedShareLedger& sh = qc_cnt.gap_sig_share;
    CoresumedGroups share_groups;
    std::set<LosId> share_answered;   // lines of sight the share passed to the responder
    if (qc.coresumedShareActive()) {
        share_groups = coresumedGroups(raw, qc);
        for (const auto& [los, grp] : share_groups) {
            const int i = los.station.index;
            if (grp.n_signals < 2 || i < 0 || i < i_lo || i >= n_sta_total
                || i >= static_cast<int>(stations.size()))
                continue;
            if (i < static_cast<int>(present.size()) && !present[i]) continue;
            ++sh.los_coresumed;
        }
    }
    // Station-ordered by the qualify pass, and this walk touches only station
    // i's own StateManager plus the two epoch-scoped sets, so it is
    // deterministic.
    for (const auto& g : raw.sig_resumptions) {
        const int i = g.station;
        if (i < 0 || i >= n_sta_total || i >= static_cast<int>(stations.size())) continue;
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        const std::string& sta_name = stations[static_cast<std::size_t>(i)].name;
        // --- the verdict, the row-level ladder applied to one signal ---------
        // `lli` is the LLI of this signal at the return epoch, not of the row's
        // primary pair: in the measured field case only the signal that never
        // dropped carried the flag, so reading the pair would attribute the
        // receiver's verdict to the wrong band.
        const bool lli_set = qc.gap_reentry_lli && (g.lli != 0);
        const bool unmeas  = qc.gap_reentry_per_signal_unmeasurable && !g.measurable;
        // The decoupling edge (invariant #7): every per-signal re-entry gets an
        // event, clean ones included, so the tier can be audited for what it
        // declined to act on as well as for its actions.
        const bool pl_bridged = [&] {
            const auto cit = net::getArc(arcs, std::make_tuple(i, g.satellite, g.signal), &net::ArcState::amb);
            if (cit == nullptr) return false;
            const double gap_s = pl_now_s - cit->last_seen_s;
            return gap_s >= 0.0 && gap_s <= pl_max_gap_s;
        }();
        traces.emitEvent(se.time, "T0_gap", "sig_resume", g.satellite, sta_name,
                         g.measurable ? std::optional<double>(g.d_gf) : std::nullopt,
                         g.measurable ? std::optional<double>(g.gate_gf) : std::nullopt,
                         g.shifted ? "shifted" : (g.measurable ? "clean" : "unmeas"),
                         fmt::format("{};gap_ep={};partner={};through={};lli={};"
                                     "carry={}",
                                     g.signal, g.gap_epochs,
                                     g.partner.empty() ? std::string("none") : g.partner,
                                     g.partner_through ? 1 : 0, g.lli,
                                     pl_bridged ? "bridged" : "fresh"));
        // The co-resumed share: a signal with no verdict of its own takes the
        // verdict of a co-resumed sibling, if one was judged.  Null when the share
        // is not active (the group map is then empty).  A signal an earlier drain
        // already freed this epoch is left as clean: a second response would add
        // nothing to the solve and count and trace the arc twice.
        const CoresumedGroup* share =
            (!g.shifted && !lli_set && !unmeas)
                ? coresumedShareFor(share_groups, i, g.satellite)
                : nullptr;
        if (share != nullptr) {
            ++sh.signals;
            if (gapReentryAlreadyFreed(relevel, i, g.satellite, g.signal)) {
                ++sh.already_freed;
                share = nullptr;
            }
        }
        if (!g.shifted && !lli_set && !unmeas && share == nullptr) {
            // Clean: bridged by design, and recorded so the unified census (R4)
            // can separate it from an arc nothing looked at.
            sidRecordOutcome(sid_out, i, g.satellite, g.signal,
                             ArcReentryOutcome::BridgedVerdict);
            continue;
        }
        ++led.verdicts;
        if (i < i_lo) {                                     // gauge-pinned legacy pivot
            ++led.pivot_skipped;
            if (share != nullptr) ++sh.skipped;
            continue;
        }
        // With the ledger armed, the population is the ledger's (Card SID phase
        // 3).  A raw per-signal resumption whose arc is not in the admitted
        // population (the estimator never lost the row, or the gap outran the
        // carry horizon) is still detected, given a verdict and traced, but gets
        // no response.  A shared verdict is a verdict here too.
        if (ledger_driven
            && !sid_pop.count(std::make_tuple(i, g.satellite, g.signal))) {
            if (share != nullptr) ++sh.skipped;
            continue;
        }
        // The share answers this line of sight (counted once per line of sight,
        // with whether its trigger was a carried signal).
        if (share != nullptr) {
            const std::optional<LosId> los = coresumedLos(i, g.satellite);
            if (los && share_answered.insert(*los).second) {
                ++sh.los_answered;
                if (!gapReentryCarryProtectable(arcs, i, g.satellite,
                                                share->trigger->signal, pl_now_s,
                                                pl_max_gap_s))
                    ++sh.los_trigger_uncarried;
            }
        }
        std::string cause_txt;
        if (share != nullptr) {
            cause_txt = "shared";
        } else {
            const auto add = [&cause_txt](const char* t) {
                if (!cause_txt.empty()) cause_txt += '|';
                cause_txt += t;
            };
            if (g.shifted) add("shifted");
            if (lli_set)   add("lli");
            if (unmeas)    add("unmeas");
        }
        // A shared free is bucketed under the trigger's primary cause, so the
        // tier's freed buckets still partition its reached arcs.
        const GapCause primary = share != nullptr
                                     ? share->trigger_v.primary
                                     : (g.shifted ? GapCause::Shifted
                                                  : (lli_set ? GapCause::Lli
                                                             : GapCause::Unmeas));
        // Band-scoped: only the signal that broke is freed.  The other bands of
        // this satellite tracked through, so their carried levels are still
        // valid, and freeing them would add the response cost that
        // SPEC_lane_slip_response measures (the slip cost of a whole-satellite
        // refloat is 100 % response-inflicted).
        // This tier does not feed gap_reentry_unreachable (Card GRK).  Its key is
        // the returning signal, so the tracker and the carry cannot disagree
        // about which signal names the band; a return on a signal the station
        // does not model correctly finds no carry, and the modelled same-band
        // carry it would otherwise be scored against tracked through this break.
        const GapArcOutcome out =
            gapReentryRespondArc(i, g.satellite, g.signal,
                                 *stations[static_cast<std::size_t>(i)].sm,
                                 carry_invariant, carry_warm, carry_slipped_ambs,
                                 arcs, pl_now_s, pl_max_gap_s,
                                 RelevelEpochState::kCauseGap, primary,
                                 led, qc_cnt, relevel,
                                 /*present_sigs=*/nullptr,
                                 /*count_unreachable=*/false);
        sidRecordOutcome(sid_out, i, g.satellite, g.signal,
                         out.reached ? ArcReentryOutcome::Freed
                                     : ArcReentryOutcome::NoCarry);
        if (ledger_driven) {
            if (out.reached) ++qc_cnt.ledger_reentry_freed;
            else             ++qc_cnt.ledger_reentry_no_carry;
        }
        if (share != nullptr) coresumedShareTally(sh, out, share->trigger_v.primary);
        if (!out.reached) continue;   // no carry to protect: the inject would not bridge it
        if (share != nullptr) {
            // The re-coupling edge of a shared free.  `scope=los_shared` separates
            // these rows from the tier's own `scope=signal` rows, and the trigger
            // fields name the co-resumed signal whose verdict this one took.  The
            // dGF field is this signal's own level test, which read clean.
            traces.emitEvent(se.time, "T0_gap", "reentry_relevel", g.satellite, sta_name,
                             g.measurable ? std::optional<double>(g.d_gf) : std::nullopt,
                             g.measurable ? std::optional<double>(g.gate_gf) : std::nullopt,
                             out.action,
                             fmt::format("{};scope=los_shared;gap_ep={};dGF={};partner={};"
                                         "through={};lli={};cause={};trigger={};"
                                         "trigger_cause={};trigger_dGF={};carry={};state={}",
                                         g.signal, g.gap_epochs,
                                         g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                      : std::string{},
                                         g.partner.empty() ? std::string("none") : g.partner,
                                         g.partner_through ? 1 : 0, g.lli, cause_txt,
                                         share->trigger->signal, share->trigger_v.cause_txt,
                                         share->trigger->measurable
                                             ? fmt::format("{:.4f}", share->trigger->d_gf)
                                             : std::string{},
                                         out.pl_bridged ? "pl" : "dd",
                                         out.state_live ? "live" : "absent"));
        } else {
            // The re-coupling edge: `scope=signal` separates these rows from the
            // row-level tier's in network_events.csv, so a grader can count the two
            // populations apart without re-deriving them.
            traces.emitEvent(se.time, "T0_gap", "reentry_relevel", g.satellite, sta_name,
                             g.measurable ? std::optional<double>(g.d_gf) : std::nullopt,
                             g.measurable ? std::optional<double>(g.gate_gf) : std::nullopt,
                             out.action,
                             fmt::format("{};scope=signal;gap_ep={};dGF={};partner={};"
                                         "through={};lli={};cause={};carry={};state={}",
                                         g.signal, g.gap_epochs,
                                         g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                      : std::string{},
                                         g.partner.empty() ? std::string("none") : g.partner,
                                         g.partner_through ? 1 : 0, g.lli, cause_txt,
                                         out.pl_bridged ? "pl" : "dd",
                                         out.state_live ? "live" : "absent"));
        }
        if (carry_warm) gapReentryVoidPends(i, g.satellite, relevel_pend, qc_cnt);
        if (++n_resp <= 12)
            detail_log += fmt::format("{}{}/{}/{} [{}]",
                                      detail_log.empty() ? "" : ", ",
                                      sta_name, g.satellite, g.signal,
                                      share != nullptr
                                          ? fmt::format("shared<-{}",
                                                        share->trigger->signal)
                                          : std::string(gapCauseName(primary)));
    }
    if (n_resp > 0)
        log->info("Network[gap-reentry/signal]: TOW {:.1f} marked {} arc(s) slipped at a "
                  "PER-SIGNAL re-entry{}.  The satellite stayed present on another signal, "
                  "so the row-level tier saw an adjacent predecessor and no detector covered "
                  "the return epoch [{}]",
                  se.time.tow, n_resp,
                  carry_warm ? (relevel ? "" : " (relevel_free_epoch OFF; mark cannot deliver "
                                               "the free epoch: counted, not acted)")
                             : " (carry cold; diagnostic)",
                  detail_log);
}

// Setup builder (card NOM3 step EP4); documented at its declaration in network/slip/NetworkSlipQc.hpp.
void networkSlipQcConfigure(NetworkRun& run) {
    // ---- From the run (card NOM3 step EP4; generated by scripts/nom_ep4_cut.py, each line
    //      checked by scripts/nom_ep4_verify.py). ----
    const GnssConfig& config_ = run.config;
    const NetworkConfig& nc = run.nc;
    std::vector<NetStation>& stations = run.setup.stations;
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    // ---- End of the bindings ----
    // Slip-QC hardening channel: knobs +
    // per-station cross-epoch state + this epoch's raw evidence + run-end
    // counters, threaded by reference into the PASS-A slip seam and the
    // assembly pre-filter.  Sized once here so the per-station EWMAs survive
    // the whole run.
    const auto& sq = nc.slip_qc;
    auto& sp = slip_qc.params;
    sp.lli_corroborate     = sq.lli_corroborate;
    sp.chatter_frac_hi     = sq.lli_chatter_frac_hi;
    sp.chatter_frac_lo     = sq.lli_chatter_frac_lo;
    sp.chatter_min_epochs  = sq.lli_min_epochs;
    sp.chatter_alpha       = sq.lli_ewma_alpha;
    sp.k_mw                = sq.lli_k_mw;
    sp.k_gf                = sq.lli_k_gf;
    sp.mw_floor_cyc        = sq.lli_mw_floor_cyc;
    sp.gf_floor_m          = sq.lli_gf_floor_m;
    sp.step_alpha          = sq.lli_step_alpha;
    sp.gap_instrument      = sq.gap_instrument;
    // The gap re-entry response consumes the same GapResumption records as
    // the instrument, so it arms the resumption collection on its own: the
    // response works without the diagnostic armed, and turning the diagnostic
    // off does not disarm it (card G, §G3/W1).
    sp.gap_reentry             = sq.gap_reentry;
    sp.gap_reentry_lli         = sq.gap_reentry_lli;
    sp.gap_reentry_unmeasurable = sq.gap_reentry_unmeasurable;
    // Per-signal rider of the same response (card GRS).  It also arms the
    // per-signal half of the compute and qualify passes, so a run with it off
    // never builds or reads a per-signal list.
    sp.gap_reentry_per_signal  = sq.gap_reentry_per_signal;
    sp.gap_reentry_per_signal_unmeasurable = sq.gap_reentry_per_signal_unmeasurable;
    // The co-resumed share of the per-signal tier: a verdict on one returning
    // signal of a line of sight frees its co-resumed siblings.  It reads the
    // rider's list and arms nothing of its own.
    sp.gap_reentry_coresumed_share = sq.gap_reentry_coresumed_share;
    // The arc-level tracker takes its per-band signal from the model, so the
    // tracker, the carried ambiguity key and the gap re-entry response's
    // lookup name the same signal.  Read at the compute pass only (card GRK).
    sp.tracker_modeled_first   = sq.tracker_modeled_first;
    // With the ledger re-entry armed, the response's population is the arc
    // ledger's admitted lifecycle, and the raw tiers become the verdict source,
    // looked up per arc.  The loader already refuses the knob without the roster, the roster
    // read and the row-level response, so no interlock is re-tested here.
    // Unarmed, both drains stay on their raw-driven path (card SID phase 3).
    sp.ledger_reentry          = config_.networkLedgerReentry();
    sp.ledger_reentry_refloat  =
        config_.networkLedgerReentryNoVerdict() == LedgerReentryNoVerdict::Refloat;
    // The GF-row capture needs the qualified raw verdicts (bridge v3 E4).
    sp.bridge_gf           = config_.networkCycleSlipRelevelBridgeGf();
    // The CS-GF authority has the same need.  The LV-6 census arms its advance:
    // the kernel is instrumentation (SPEC_level_authority_internal §3).  Arming
    // iono_cs flips cardsArmed(), so census A/B pairs must pin
    // cycle_slip.gap_instrument in both arms.
    sp.iono_cs             = config_.networkLevelAuthorityCsgf();
    // The arc-scoped GF jump trigger (card ARL) reads the raw rows and keeps its own
    // per-arc steps, so it arms the compute pass (anyConsumer) and not the qualify
    // pass.
    sp.gf_jump.armed       = sq.gf_arc_jump;
    sp.gf_jump.floor_m     = sq.gf_arc_jump_m;
    sp.gf_jump.k           = sq.gf_arc_jump_k;
    sp.gf_jump.window      = sq.gf_arc_jump_window;
    sp.gf_jump.min_count   = sq.gf_arc_jump_min_count;
    slip_qc.hist.resize(stations.size());
}

// Setup builder (card NOM3 step EP4); documented at its declaration in network/slip/NetworkSlipQc.hpp.
void networkSlipQcArmWavelengthProvider(NetworkRun& run, LogManager& log_manager_) {
    // ---- From the run (card NOM3 step EP4; generated by scripts/nom_ep4_cut.py, each line
    //      checked by scripts/nom_ep4_verify.py). ----
    NetworkRunTraces& traces = run.traces;
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    auto& slip_raw_trace       = traces.slip_raw_trace;
    // ---- End of the bindings ----
    // Wavelength provider for the raw-slip path, gated on the streams openAll just
    // opened.  It is built here because it needs log_manager_, which the traces
    // unit does not hold.  The raw-slip provider serves three consumers (the trace, card L and card G) and is built when any
    // of them is armed, per spec §1: "compute once per epoch when any consumer
    // needs it".  With none armed it stays null and the raw-observable path is
    // skipped.
    if (slip_qc.params.anyConsumer(static_cast<bool>(slip_raw_trace)))
        slip_qc.utils = std::make_unique<PppUtils>(log_manager_);
}

} // namespace GnssProcessor

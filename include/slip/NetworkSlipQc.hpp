#pragma once
/**
 * @file NetworkSlipQc.hpp
 * @brief Network run-loop slip QC: the PASS-A buffer seam plus the serial
 *        T0 repair drain, MW-detect/LLI refloat drain, and IF-phase
 *        triple-difference driver, with their pure helpers.
 *
 * Seeded at decomposition INC-2 (ifElevMap / parseIfPairResetKey) and
 * completed at INC-6 (2026-07-16), which relocated the SB_SLIP block's
 * serial drains VERBATIM out of ApplicationNetwork.cpp (see
 * docs/network_decomposition_plan.md).  The PASS-A parallel
 * detection loop that FILLS PassABuffers — and the SB_SLIP D0b timer
 * bracket around scan + drains — remain in the run TU until INC-7 moves
 * the loop into the Assembly TU.
 *
 * Threading contract: everything declared here is SERIAL, station-ordered
 * (deterministic).  Cross-epoch slip state deliberately stays where it
 * lives — detector internals and the per-(station,pair) IF EWMA scale
 * (NetStation::if_pair_scale) persist inside NetStation; the phase-leveling
 * carry map persists in NetworkCarryState.  These functions own NO state.
 */

#include "common/Constants.hpp"
#include "filter/NetworkRunTypes.hpp"   // NetStation, LocalCarry, TurboEditResolver::SignalRepair, PppObservation (transitive)
#include "logging/NetworkRunTraces.hpp" // NetworkRunTraces (emitEvent, if_slip_trace), FlagRate
#include "slip/NetworkRawObservables.hpp" // RawSlipEpoch/History, SlipQcParams/Counters (Cards E/L/G)

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct SyncedEpoch;

/// Elevation mapping for the IF-phase slip stochastic model: the multiplicative
/// factor by which between-epoch double-differenced carrier-phase noise inflates
/// toward the horizon.  A residual at elevation @p el_deg is normalized to a
/// zenith-equivalent by DIVIDING by this factor (≡ multiplying by sin el); the
/// empirical scale (pooled or per-station EWMA) is re-inflated by it per satellite
/// when thresholding, so ONE scale is valid across the whole sky.  1/sin(el) is the
/// standard first-order phase-noise shape; clamped at 5° so it cannot blow up at the
/// (already elevation-masked) horizon.  A future refinement is a fitted a+b/sin(el).
inline double ifElevMap(double el_deg) {
    constexpr double kMinElDeg = 5.0;
    const double el_rad = std::max(el_deg, kMinElDeg) * (Constants::PI / 180.0);
    const double s = std::sin(el_rad);
    return (s > 1e-3) ? (1.0 / s) : 1.0;
}

/// Parse an IfPhaseSlipDetector reset key ("G01_L1C_L2W") into the satellite and
/// the two band signals of the IF pair it was formed from.  Returns false on a
/// malformed key.  The network carries per-band ambiguities, so a flagged IF pair
/// must refloat BOTH bands — the IF combination cannot localize which one slipped.
inline bool parseIfPairResetKey(const std::string& key, std::string& sat,
                                std::string& sig1, std::string& sig2) {
    const auto p1 = key.find('_');
    if (p1 == std::string::npos || p1 == 0) return false;
    const auto p2 = key.find('_', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1 || p2 + 1 >= key.size()) return false;
    sat  = key.substr(0, p1);
    sig1 = key.substr(p1 + 1, p2 - p1 - 1);
    sig2 = key.substr(p2 + 1);
    return !sat.empty() && !sig1.empty() && !sig2.empty();
}

/**
 * @brief PASS-A slip-scan seam: the per-station buffers the parallel detection
 *        loop fills (one index-addressed slot per station — no shared write)
 *        and the serial drains below consume in station order.
 *
 * The constructor reproduces the former brace-scope initializations exactly
 * (size = stations.size(), a_ok zero-filled).  Freshly constructed every
 * epoch — never hoisted for reuse.
 */
struct PassABuffers {
    /// Per-station PASS-A geometry success flag, folded into present[] in a
    /// serial pass.  char, not bool: std::vector<bool> packs bits, so
    /// concurrent writes to adjacent indices would race.
    std::vector<char>        a_ok;
    /// Per-station PASS-A exception text (an exception must never escape an
    /// OpenMP structured block — caught per iteration, logged serially).
    std::vector<std::string> a_err;
    /// Phase-leveling T0: per-station integer slip repairs, produced in the
    /// parallel loop into a private slot, applied to the shared carry map in
    /// the SERIAL station-ordered drain (networkSlipDrainT0Repairs).
    std::vector<std::vector<TurboEditResolver::SignalRepair>> sta_repairs;
    /// IF-phase slip detector output: per-station flagged IF-pair reset keys
    /// ("G01_L1C_L2W"), consumed in a serial station-ordered marking pass
    /// (networkSlipIfTdDriver — refsat common-mode fold).
    std::vector<std::vector<std::string>> sta_if_resets;
    /// MW DETECTOR (mw_detect) output: confident wide-lane integer slips to
    /// REFLOAT, consumed in the serial MW-detect drain
    /// (networkSlipDrainMwRefloat).  Empty when turbo_edit runs the
    /// integer-repair path instead, or when mw_detect is off.  Widened from a
    /// (sat, sig1, sig2) tuple (SPEC_slip_relevel_walk §6): the confident WL
    /// integer, its fractional confidence, and the WL wavelength now travel to
    /// the drain so the delta bridge can use them — the fill gates
    /// (slip_wl_known && != 0 && frac <= frac_max) are unchanged, and NEVER
    /// fill from slip_wl_float (candidate-selector-only contract,
    /// MwSlipDetector.hpp).
    struct MwRefloatCand {
        std::string sat, sig1, sig2;
        int    dn_wl{0};        ///< Confident ZD wide-lane integer (cycles).
        double frac{0.0};       ///< MW fractional distance (cycles) — confidence.
        double lambda_wl{0.0};  ///< Wide-lane wavelength (m).
    };
    std::vector<std::vector<MwRefloatCand>> sta_mw_refloat;

    explicit PassABuffers(std::size_t n)
        : a_ok(n, 0), a_err(n), sta_repairs(n), sta_if_resets(n), sta_mw_refloat(n) {}
};

/**
 * @brief Phase-leveling T0 serial repair drain: apply the PASS-A integer slip
 *        repairs to the phase-leveling carry, BEFORE PASS B reads the carry
 *        to build the prior.
 *
 * Policy (BIA-less network, data-driven): REPAIR a confident, sane-magnitude
 * integer by shifting the carried value continuously (variance unchanged,
 * covariance-preserving); LEAVE a low-confidence detection untouched — the
 * float wide-lane's emitted-but-low-confidence repairs are predominantly
 * false positives, and acting on them measurably degrades the products.  The
 * pivot (stations[0]) is never touched.  Full rationale in the relocated
 * banner comment in NetworkSlipQc.cpp.
 *
 * @param pa           PASS-A buffers (reads sta_repairs).
 * @param config_      Config (TurboEdit frac/dN acceptance gates read inside).
 * @param stations     Station roster (names for logging only).
 * @param present      Per-station PASS-A presence (absent stations skipped).
 * @param i_lo         First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total  Station count.
 * @param local_carry  Phase-leveling carry map — MUTATED (value_abs shifted by
 *                     dN·λ on accepted repairs).  The ctx.carry.local_carry
 *                     instance; pass the map itself, never a copy.
 * @param se           Current synced epoch (TOW for log lines).
 * @param log          Run logger.
 */
void networkSlipDrainT0Repairs(const PassABuffers& pa, const GnssConfig& config_,
                               const std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>& local_carry,
                               const SyncedEpoch& se,
                               const std::shared_ptr<spdlog::logger>& log);

/**
 * @brief MW-detect / LLI refloat drain: REFLOAT ambiguities flagged by a
 *        confident PASS-A wide-lane integer jump or by the receiver's own LLI
 *        loss-of-lock bit — never applying an integer, so a wrong wide-lane
 *        integer can never bias the gauge.
 *
 * Gated INSIDE on network.cycle_slip.mw_detect (call unconditionally).  The
 * refloat_signal lambda lives here (deliberately NOT merged with the IF
 * driver's refloat_pair).  Also the Card-4 C3c feed: every flagged
 * (station, ambiguity) is inserted into @p carry_slipped_ambs from epoch 1,
 * independent of the legacy carry's warm gate.
 *
 * ### Card E (2026-07-21) — detections are emitted when the carry is COLD
 *
 * `refloat_signal` now reports MATCHED and REFLOATED as separate facts.
 * Emission and the n_mw / n_lli log counters key on the DETECTION; refloat
 * accounting is a separate counter and stays warm-only.  A cold run therefore
 * writes `mw_wl_slip` / `lli_slip` rows with `action=diagnostic` — mirroring
 * what the IF-TD driver has always done — instead of writing nothing at all
 * while still feeding the C3c funnel.  Strictly numerically inert: neither
 * markAmbiguityPendingReseed nor the @p carry_slipped_ambs insert moved.
 *
 * ### Card L (2026-07-21) — the LLI arm is QUALIFIED, never excluded
 *
 * An LLI assertion acts when it is CORROBORATED by an empirical MW or GF step
 * (decisive at any station — a real multi-satellite re-lock always survives),
 * or when the station's own LLI behaviour is still credible (a two-state,
 * hysteretic EWMA of its LLI-set fraction).  Only the third case — an
 * uncorroborated assertion at a station whose LLI is demonstrably
 * non-credible — is downgraded, and then it is emitted with
 * `action=suppressed` and counted, never silently dropped.  Because the whole
 * `refloat_signal` call is skipped for a suppressed assertion, the C3c funnel
 * insert obeys the same qualification as the pending-reseed path (spec M5).
 * The qualification is inert unless @c SlipQcParams::lli_corroborate is armed.
 *
 * @param pa                 PASS-A buffers (reads sta_mw_refloat).
 * @param config_            Config (mw_detect gate read inside).
 * @param stations           Station roster — StateManager pending-reseed marks.
 * @param present            Per-station PASS-A presence.
 * @param modeled            Per-station modeled observations (LLI flag scan).
 * @param i_lo               First non-pivot station index.
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry armed (C3c feed gate).
 * @param carry_warm         Legacy phase-leveling carry past its warmup gate
 *                           (refloat is meaningful only when warm; cold, the
 *                           detection is diagnostic).
 * @param carry_slipped_ambs THE per-epoch C3c slip-event set — the single
 *                           run-TU instance by NON-CONST reference.  A value
 *                           copy anywhere on this path silently disables the
 *                           Card-4 slip funnel (standing invariant #1).
 * @param raw                This epoch's shared raw observables + step verdicts
 *                           (Card L evidence).  @c computed==false ⇒ no
 *                           evidence available ⇒ the qualification is skipped
 *                           and LLI stays decisive.
 * @param qc                 Card L / G knob snapshot (all-OFF ⇒ legacy behaviour).
 * @param raw_hist           Per-station cross-epoch slip-QC state — the
 *                           per-station LLI accounting for the M4
 *                           top-offenders line is tallied here.
 * @param qc_cnt             Run-end slip-QC counters — accumulated into.
 * @param se                 Current synced epoch (event rows / log lines).
 * @param traces             Trace unit (T0_mw_detect event rows).
 * @param log                Run logger.
 *
 * ### Datum-station slip detect (network.cycle_slip.datum_station_detect)
 *
 * The three trailing parameters arm the §9.7.10 datum-station path and are
 * passed together or not at all (all nullptr / null-equivalent ⇒ OFF ⇒
 * byte-identical; the caller passes them only under the LEGACY pivot datum
 * with the toggle armed).  With them armed the loop starts at station 0, and
 * a station-0 MW candidate or qualified LLI assertion takes the datum
 * re-reference path instead of refloat_signal: the satellite is queued in
 * @p datum_pb_reref_sats (consumed at this epoch's stash — hard PB carry
 * skip) and @p datum_amb_reref_sats (consumed at the FIRST drain after that
 * skip — the block at the top of this function, which pending-reseeds the
 * sat's ambiguities at EVERY non-pivot station, erases their local carry,
 * feeds the Card-4 C3c funnel, inserts the sat into @p datum_mark_sats for
 * a per-sat disc mark at the next emit, and emits the datum_reref event).
 *
 * @param datum_pb_reref_sats  Per-sat PhaseBias skip-stash queue
 *                             (ctx.carry.datum_reref_pb_sats) — nullptr ⇒ OFF.
 * @param datum_amb_reref_sats Per-sat deferred all-station ambiguity reseed
 *                             queue (ctx.carry.datum_reref_amb_sats).
 * @param datum_mark_sats      Per-sat disc-mark queue at next emit
 *                             (ctx.carry.refsat_mark_sats — the Edge-1 piece-2
 *                             mark-and-serialize path).
 * @param datum_local_carry    Phase-leveling carry map for the belt-and-
 *                             suspenders erase on re-reference (the
 *                             ctx.carry.local_carry instance).
 */
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
                               std::set<std::string>* datum_pb_reref_sats = nullptr,
                               std::set<std::string>* datum_amb_reref_sats = nullptr,
                               std::set<std::string>* datum_mark_sats = nullptr,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>* datum_local_carry = nullptr,
                               RelevelEpochState* relevel = nullptr,
                               std::map<std::tuple<int, std::string, std::string>,
                                        RelevelPend>* relevel_pend = nullptr,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>* bridge_local_carry = nullptr);

/**
 * @brief SPEC_slip_relevel_walk §6 — float 2×2 delta-bridge solve for one
 *        (MW, IF-TD) detector pair.
 *
 * Given the confident zero-differenced wide-lane integer @f$\Delta N_{WL} =
 * \Delta N_1 - \Delta N_2@f$ (cycles) and the measured ionosphere-free phase
 * step @f$\delta_{IF} = \beta_1\lambda_1... = \beta_1 \Delta N_1 - \beta_2
 * \Delta N_2@f$ (metres, @f$\beta_j = \lambda_{WL} f_j/(f_1+f_2)@f$ with the
 * identity @f$\beta_1-\beta_2 = \lambda_{IF}@f$), solve the per-band deltas as
 * FLOATS — a bridge, never an integer fix, so a wrong wide-lane integer can
 * bias but never lock the gauge, and the propagated variance keeps the bridged
 * prior loose enough for the data to re-pull it (invariant #1: the arc stays
 * catchable).  The band frequencies are derived from the two wavelengths alone
 * (@f$f_1 = \tfrac{c}{2}(1/\lambda_{WL} + 1/\lambda_{IF})@f$, exact per
 * satellite — GLONASS FDMA included), so the kernel needs no signal tables.
 *
 * @param dn_wl_cyc     Confident ZD wide-lane integer (cycles).
 * @param sigma_wl_cyc  Its 1σ confidence (cycles).
 * @param dif_m         Measured IF phase step, common-mode removed (m).
 * @param sigma_if_m    Its 1σ (m) — the detector's own pooled scale.
 * @param lambda_wl_m   Wide-lane wavelength (m).
 * @param lambda_if_m   (1,1) IF wavelength c/(f1+f2) (m).
 * @param max_abs_cyc   Reject when either |ΔN_j| exceeds this (cycles).
 * @param max_sigma_cyc Reject when either propagated σ(ΔN_j) exceeds this.
 * @return ok=false on any reject; else the per-band float deltas, the metre
 *         shifts λ_j·ΔN_j, and the per-band shift variances (m²).
 */
struct DeltaBridgeSolve {
    bool   ok{false};
    double dn1_cyc{0}, dn2_cyc{0};
    double shift1_m{0}, shift2_m{0};
    double var1_m2{0}, var2_m2{0};
};
DeltaBridgeSolve networkSlipDeltaBridgeSolve(double dn_wl_cyc, double sigma_wl_cyc,
                                             double dif_m, double sigma_if_m,
                                             double lambda_wl_m, double lambda_if_m,
                                             double max_abs_cyc, double max_sigma_cyc);

/**
 * @brief Bridge v2 (pend-and-replace) — execute one pended replace against the
 *        live carry map.  Pure function of its arguments (unit-test seam).
 *
 * Solves the 2×2 from the pend's (δIF, σIF) and its arrived MW integer, then
 * gates on the §8.1 audit's CORROBORATION: the bridged level (pre_slip + shift)
 * must agree with the CURRENT carry value — the interim v1 free re-level —
 * within @p corrob_cyc cycles per band (correct bridges err ≤ 0.5 cyc against
 * truth while wrong ones err 6–113 cyc; the free level sits within ~2 cyc, so
 * agreement separates the populations).  On Applied the carry values are
 * REPLACED (value = pre + shift, var = pre_var + bridge var; coupling and
 * last_seen untouched — the local-step mutation discipline).  Every non-Applied
 * outcome leaves the carry exactly as found (the arc keeps its free re-level).
 */
enum class BridgeReplaceOutcome { Applied, KernelReject, NoCarry, CorrobReject };
BridgeReplaceOutcome networkSlipBridgeReplace(
    const RelevelPend& pd, int sta, const std::string& sat,
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
    DeltaBridgeSolve* solved = nullptr);

/**
 * @brief Bridge v3 — staged closed-form cascade over the pend's equation menu
 *        (SPEC_slip_relevel_walk §8.2 follow-on: the better second equation).
 *
 * Stage 1 resolves the IF-pair plane (ΔN1, ΔN2):
 *  - E2 route (@c has_dn): the v2 2×2 from the MW wide-lane integer — kept
 *    verbatim (the integer, when it arrives, is the stronger equation).  When
 *    the GF row is ALSO present, the E4 measurement over-determines the plane
 *    and the standardized plane residual @f$|d_{GF} - (\lambda_1\Delta N_1 -
 *    \lambda_2\Delta N_2)| / \sigma@f$ screens a wrong detector-rounded
 *    integer (±1 WL sits 2.5–5σ off the float plane) — reject at
 *    @c kBridgePlaneResidMax.
 *  - E1+E4 route (@c has_gf, no integer yet): solve
 *    @f$[\beta_1, -\beta_2; \lambda_1, -\lambda_2][\Delta N_1;\Delta N_2] =
 *    [\delta_{IF}; d_{GF}]@f$ — same-epoch, float-only, zero
 *    integer-hypothesis risk.  det ≈ −0.046 m² (GPS) / −0.048 (GAL); at
 *    cm-class inputs σ(ΔN) ≈ 0.15–0.3 cyc.  The two measurements share the
 *    same band TDs, so the propagation carries an off-diagonal
 *    @f$\rho\,\sigma_{IF}\sigma_{GF}@f$ (phase-noise limit ρ ≈ +0.97, and the
 *    solve coefficients have OPPOSITE signs, so the shared noise partially
 *    CANCELS: independent propagation (ρ = 0) OVERSTATES σ(ΔN) — up to ~3.7×
 *    in the pure-phase limit, unit-pinned — costing yield through the σ cap,
 *    never safety.  ρ must never exceed the physical limit: an over-set ρ
 *    UNDER-states σ, which is the dangerous direction.  0.6 is the shipped
 *    conservative middle).
 * Stage 2 (@c has_ewl) chains band 3: ΔN3 = ΔN_shared + dn_ewl_b3 (the pend
 * normalized the candidate's frequency-sorted sign at fill), var3 =
 * var_shared + σ_EWL² (cycles), λ3 derived from (λ_shared, λ_EWL) alone.
 * FLOAT throughout — the kernel never rounds (rounding its output re-imports
 * the mw_only wrongness behind an honest-looking σ).
 */
struct BridgeV3Solve {
    bool   ok{false};
    bool   has3{false};            ///< Band-3 chained (EWL slot present + stage 1 ok).
    bool   used_gf{false};         ///< Stage 1 took the E1+E4 route (no MW integer).
    double dn1_cyc{0}, dn2_cyc{0}, dn3_cyc{0};
    double shift1_m{0}, shift2_m{0}, shift3_m{0};
    double var1_m2{0}, var2_m2{0}, var3_m2{0};
    double lam3_m{0};              ///< Band-3 wavelength (for the corroboration).
    double plane_resid_sigma{-1.0};///< E2-vs-E4 overdetermination residual (σ); <0 = not formed.
};
/// Conservative E1/E4 error correlation (shared band-TD content; phase limit 0.97).
inline constexpr double kBridgeGfIfRho = 0.6;
/// Plane-residual reject threshold (σ) for a wrong detector-rounded WL integer.
inline constexpr double kBridgePlaneResidMax = 4.0;
BridgeV3Solve networkSlipBridgeV3Solve(const RelevelPend& pd, double rho_if_gf,
                                       double max_abs_cyc, double max_sigma_cyc);

/**
 * @brief Bridge v3 replace — the v2 outcome ladder over up to THREE bands.
 *
 * Same discipline as @ref networkSlipBridgeReplace: kernel caps, then per-band
 * free-level corroboration (band 3 included when solved and its pre-slip carry
 * was captured at the FLAG epoch), then value+var-only mutation.  NO partial
 * apply — a corroboration failure on any solved band rejects the whole bridge
 * (a half-replaced LOS would mix pre-slip and free-re-level frames).
 */
BridgeReplaceOutcome networkSlipBridgeV3Replace(
    const RelevelPend& pd, int sta, const std::string& sat,
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    double rho_if_gf, double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
    BridgeV3Solve* solved = nullptr);

/**
 * @brief Bridge v3 E3: fill a pend's EWL slot from a confident MW candidate
 *        whose pair shares EXACTLY ONE band with the pend's IF pair.
 *
 * Normalizes the integer to ΔN(band3) − ΔN(shared band) regardless of the
 * candidate's frequency-sorted pair order — the panel's #1 defect: the GAL EWL
 * arrives as (L7Q,L5Q) = ΔN3 − ΔN2 while GPS's (L2W,L5Q) = ΔN2 − ΔN3, and a
 * missed flip passes every σ cap.  Also derives the shared-band index (1 or 2
 * within the IF pair) and whether band 3 sits on the EWL pair's high-frequency
 * side (the λ3 derivation needs the sign of 1/λ_EWL).  @p sigma_floor_eff_cyc
 * is the ALREADY elevation-mapped floor — the v2 frac proxy understates the
 * single-epoch EWL σ ~100× at low elevation.  Returns false (pend untouched)
 * when the pair does not link exactly-one band, the pend already holds an EWL,
 * or the linked third band contradicts the pend's recorded sig3.
 */
bool networkSlipFillEwlSlot(RelevelPend& pd, const MwBridgeCand& cand,
                            double sigma_floor_eff_cyc);

/**
 * @brief IF-phase triple-difference slip driver: pool the PASS-A deferred
 *        ΔΔφ_IF caches across stations by signal pair, remove the network
 *        common-mode median, threshold elevation-aware against an
 *        empirical-Bayes blend of the pooled robust scale and each
 *        (station,pair)'s own EWMA scale, then refloat flagged arcs and fold
 *        clean survivors back into the EWMA (slips excluded).
 *
 * Gated INSIDE on network.cycle_slip.if_phase_td (call unconditionally); the
 * five threshold knobs are snapshot inside, algorithm banner relocated to the
 * .cpp.  The refloat_pair lambda and the PairSample POD live here verbatim.
 * The per-(station,pair) EWMA scale is cross-epoch state and stays in
 * NetStation::if_pair_scale, mutated through @p stations.  Also folds the
 * detectors' reference-satellite common-mode reset keys (pa.sta_if_resets),
 * and feeds @p carry_slipped_ambs exactly as the MW drain does (C3c).
 *
 * @param pa                 PASS-A buffers (reads sta_if_resets).
 * @param config_            Config (if_phase_td gate + 5 knobs read inside).
 * @param stations           Station roster — detector arc resets, EWMA scale
 *                           updates, StateManager pending-reseed marks.
 * @param present            Per-station PASS-A presence.
 * @param i_lo               First non-pivot station index.
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry armed (C3c feed gate).
 * @param carry_warm         Legacy carry warm gate (refloat vs diagnostic).
 * @param carry_slipped_ambs THE per-epoch C3c slip-event set by NON-CONST
 *                           reference (see networkSlipDrainMwRefloat).
 * @param trace_flag_rates   B8 flag-rate counters armed.
 * @param fr_if_slip         T1 flag-rate counter — the ctx.counters.fr_if_slip
 *                           instance by reference (feeds the teardown summary).
 * @param se                 Current synced epoch (event/trace rows, log lines).
 * @param traces             Trace unit (T1_if_slip event rows + the deep
 *                           if_slip_trace CSV stream).
 * @param log                Run logger.
 * @param refsat_mark_sats   Edge-1 piece 2 (@c network.mark_refsat_reset_disc,
 *                           default OFF ⇒ nullptr ⇒ byte-identical).  When
 *                           non-null, every satellite whose REFERENCE-path arc is
 *                           reset here is inserted for a per-satellite
 *                           discontinuity mark at its next actual emit
 *                           (@c NetworkSolPrep drains it through the same
 *                           mark-and-serialize path as the B1 retro-bump, so it
 *                           lands as an @c R record).
 *
 *                           Why an EXPLICIT mark rather than relying on the
 *                           emitter's replay-safe bias-jump test (the reason
 *                           eq34_reattach / anchor handovers deliberately do NOT
 *                           mark — see NetworkSolPrep.cpp:787,801): most ref-path
 *                           slips DO step the emitted bias past the jump gate and
 *                           are already caught (that is the 83-reset population of
 *                           HANDOFF_edge_response_queue.md §1.1).  But SPEC §25.3
 *                           measured the excursion ONSETS as refsat_reset-enriched
 *                           5.0x with **PB value steps = 0** — i.e. the
 *                           onset-relevant subset re-references WITHOUT a value
 *                           step, so the jump test provably cannot see it.  This
 *                           mark targets exactly that subset.  It is NOT
 *                           reconstructable from the serialized epoch (replay does
 *                           not re-run slip QC), which is why it must travel as an
 *                           R record rather than being recomputed.
 */
void networkSlipIfTdDriver(const PassABuffers& pa, const GnssConfig& config_,
                           std::vector<NetStation>& stations,
                           const std::vector<bool>& present,
                           int i_lo, int n_sta_total,
                           bool carry_invariant, bool carry_warm,
                           std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                           bool trace_flag_rates, FlagRate& fr_if_slip,
                           const SyncedEpoch& se, NetworkRunTraces& traces,
                           const std::shared_ptr<spdlog::logger>& log,
                           std::set<std::string>* refsat_mark_sats = nullptr,
                           RelevelEpochState* relevel = nullptr,
                           std::map<std::tuple<int, std::string, std::string>,
                                    LocalCarry>* bridge_local_carry = nullptr,
                           SlipQcCounters* bridge_cnt = nullptr,
                           std::map<std::tuple<int, std::string, std::string>,
                                    RelevelPend>* relevel_pend = nullptr,
                           const RawSlipEpoch* raw_epoch = nullptr,
                           const std::vector<RawSlipHistory>* raw_hist = nullptr);

} // namespace GnssProcessor

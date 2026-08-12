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
 * docs/network_decomposition_addendum_2026-07-16.md).  The PASS-A parallel
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
    /// REFLOAT, as (satellite, sig1, sig2) tuples, consumed in the serial
    /// MW-detect drain (networkSlipDrainMwRefloat).  Empty when turbo_edit
    /// runs the integer-repair path instead, or when mw_detect is off.
    std::vector<std::vector<std::tuple<std::string, std::string, std::string>>>
        sta_mw_refloat;

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
                                        LocalCarry>* datum_local_carry = nullptr);

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
                           std::set<std::string>* refsat_mark_sats = nullptr);

} // namespace GnssProcessor

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
#include "filter/NetworkInject.hpp"     // InjectArcErase / NetworkInjectState (EVENT HARNESS Study 1)
#include "logging/NetworkRunTraces.hpp" // NetworkRunTraces (emitEvent, if_slip_trace), FlagRate
#include "slip/NetworkRawObservables.hpp" // RawSlipEpoch/History, SlipQcParams/Counters (Cards E/L/G)
#include "slip/NetworkSlipOracle.hpp"   // NetworkSlipOracle (sim-only oracle slip response)
#include "slip/NetworkCommonBandScope.hpp"  // §14 common-band verdict types (drain hook)

#include <algorithm>
#include <cmath>
#include <limits>
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

/// Pooled centre and scale of ONE IF-TD signal pair, over that pair's samples
/// from every present station (steps 1–2 of @ref networkSlipIfTdDriver).
struct IfTdPairPool {
    double med{ 0.0 };     ///< Median of the RAW double differences (m) — the network common mode.
    double s_pool{ 0.0 };  ///< 1.4826·median(|z|), z = (dd−med)/ifElevMap(el) — zenith-equivalent m.
};

/// @brief Pooled IF-TD centre + robust scale for one signal pair.
///
/// Factored out of @ref networkSlipIfTdDriver so the common-band scope's
/// read-only FLAG PREVIEW (SPEC_event_harness §14 — the verdict is formed before
/// the drains, while the pooled flag is decided inside the IF-TD driver that runs
/// after them) cannot drift from the decision it previews.  Both call this.
///
/// The centre is removed in RAW space because the network common mode (frame /
/// datum wobble) is elevation-independent.  The scale EXCLUDES the constellation
/// reference satellite, whose double difference is identically zero by
/// construction and deflates the MAD by ~16 % (§6.14.5 defect 2); the fallback
/// keeps the full set when excluding would leave nothing, so a pair whose only
/// sample IS the reference gets a real scale rather than a zero that collapses
/// onto the physical floor.  Both order statistics are (n/2)-th elements, so the
/// result does not depend on the order the samples arrive in.
///
/// @param dd      Raw between-epoch double differences, one per sample (m).
/// @param el      Satellite elevation of each sample (degrees); same length as @p dd.
/// @param is_ref  Non-zero where the sample IS the constellation reference; same length.
/// @return Pooled centre and scale; @c {0,0} for an empty sample set.
inline IfTdPairPool ifTdPoolStats(const std::vector<double>& dd,
                                  const std::vector<double>& el,
                                  const std::vector<char>& is_ref) {
    IfTdPairPool out;
    if (dd.empty()) return out;
    std::vector<double> raw = dd;
    std::nth_element(raw.begin(), raw.begin() + raw.size() / 2, raw.end());
    out.med = raw[raw.size() / 2];
    std::vector<double> zabs;
    zabs.reserve(dd.size());
    for (std::size_t k = 0; k < dd.size(); ++k)
        if (!is_ref[k]) zabs.push_back(std::abs((dd[k] - out.med) / ifElevMap(el[k])));
    if (zabs.empty())
        for (std::size_t k = 0; k < dd.size(); ++k)
            zabs.push_back(std::abs((dd[k] - out.med) / ifElevMap(el[k])));
    std::nth_element(zabs.begin(), zabs.begin() + zabs.size() / 2, zabs.end());
    out.s_pool = 1.4826 * zabs[zabs.size() / 2];
    return out;
}

/// @brief Empirical-Bayes shrunk IF-TD scale for one (station, signal pair).
///
/// Step 3 of @ref networkSlipIfTdDriver, factored for the same reason as
/// @ref ifTdPoolStats.  The effective scale shrinks from the network pool prior
/// toward the station-pair's OWN EWMA as that pair accumulates clean samples, so
/// a quiet reference station and a noisy modernized-signal station each get their
/// own envelope instead of one threshold set by the majority.
///
/// @param count         Clean samples folded into this station-pair's EWMA.
/// @param ewma_scale    That EWMA (zenith-equivalent m).
/// @param s_pool        This epoch's pooled prior (zenith-equivalent m).
/// @param shrink_kappa  Shrinkage constant, in samples.
/// @return Effective scale (zenith-equivalent m); falls back to @p s_pool on a
///         degenerate (non-positive) blend, e.g. an all-equal pool.
inline double ifTdShrunkScale(long count, double ewma_scale,
                              double s_pool, double shrink_kappa) {
    const double w = static_cast<double>(count) /
                     (static_cast<double>(count) + shrink_kappa);
    double s_eff = (count > 0) ? (w * ewma_scale + (1.0 - w) * s_pool) : s_pool;
    if (!(s_eff > 0.0)) s_eff = s_pool;  // degenerate all-equal pool
    return s_eff;
}

/**
 * @brief §Z — the outcome of a band-scoped detector refloat decision for one arc.
 *
 * (docs/SPEC_event_harness.md §Z.)  Study 1 measured that freeing EVERY band of a
 * line of sight is what buys the persistent ~0.45-cycle level floor: with no band
 * left pinned, (ρ, I, N₁..N₃) re-draw together from one epoch of code, and the
 * station-common iono error that lands in the fresh zero-q level never leaves.
 * Leave one band pinned and it anchors the LOS's ionosphere while the freed bands
 * re-tap — the floor drops 9–29× on every band, including bands never freed.
 */
struct BandScopeDecision {
    bool        scoped{false};        ///< The scope ACTED: this drain's bands plus what was ALREADY freed this epoch cover the arc, and one survivor was spared.
    bool        single_band{false};   ///< The arc has ONE live band.  Never spared on the DETECTOR path (see @ref networkSlipBandScope).
    bool        measured{false};      ///< The kept band was chosen on RAW evidence (not the lexical fallback).
    bool        too_late{false};      ///< Every live band was ALREADY freed by an earlier drain this epoch — nothing left to pin.  A wiring defect if it is ever nonzero (see @ref networkSlipBandScope).
    std::string keep;                 ///< The band left pinned; empty when the scope did not act.
    double      keep_dn_cyc{0.0};     ///< |ΔN| of the kept band in cycles from the MW/GF decomposition; 0 when !@ref measured.
};

/**
 * @brief §Z — decide which band of an arc stays pinned when a detector implicated
 *        all of them.  PURE (no state, no I/O) — the unit-testable core.
 *
 * Returns a no-op decision (@c scoped false) whenever the implicated set leaves at
 * least one live band alone: the natural scope of MW, of a 3-band arc's IF-TD pair,
 * or of a single-band LLI already keeps an anchor, and the knob must not perturb it.
 *
 * ⚠ **The invariant is per EPOCH, not per drain** — measured, not assumed.  The
 * first Study-3 all-band leg wired the scope per drain and came out BYTE-IDENTICAL
 * to its control: an LLI on all three bands made the MW/LLI drain keep L1C, and
 * then the IF-TD driver — whose pair (L1C, L2W) does NOT cover a 3-band arc, so
 * its own scope is correctly a no-op — freed L1C anyway, and the arc ended the
 * epoch with every band freed exactly as before.  @p already_freed closes that:
 * the decision is taken against this drain's bands UNION what earlier drains have
 * already latched this epoch, and the survivor is chosen from the bands that are
 * still pinned.  Pass the epoch's freed set (`relevel->free_ambs` and/or the
 * Card-4 `carry_slipped_ambs`, filtered to this arc) — empty is the first drain's
 * correct input.
 *
 * When the implicated set DOES cover the arc the band with the smallest raw
 * per-band evidence is kept.  That evidence is the MW/GF decomposition of the
 * arc's primary pair: with ΔMW in wide-lane cycles equal to Δ(N₁−N₂) and ΔGF in
 * metres equal to λ₁ΔN₁ − λ₂ΔN₂ (the one-epoch Δiono term is what the GF detector
 * already assumes negligible), the two band deltas separate exactly —
 * ΔN₁ = (ΔGF − λ₂ΔMW)/(λ₁−λ₂), ΔN₂ = (ΔGF − λ₁ΔMW)/(λ₁−λ₂).  Bands outside that
 * pair carry NO evidence and are never preferred over a measured one (absence of
 * evidence is not evidence of innocence — standing invariant #1).  With no usable
 * evidence at all the pin falls back to the lexically lowest live band, which is
 * deterministic and is also the shortest wavelength, i.e. the smallest metric level
 * error per retained cycle.
 *
 * ⚠ **This is the one place a truly-slipped band can stay pinned** — a receiver
 * loss of lock on the whole satellite implicates every band honestly, and the
 * fallback keeps one of them anyway.  That is a deliberate trade against the
 * measured cost of freeing them all, not a claim that the kept band is clean; the
 * honest handling of that case is the lane-preserving refloat (an EWL-MW snap that
 * re-levels the band rather than keeping or freeing it) named in
 * docs/SPEC_lane_slip_response.md §7, which is out of this card's scope.  The
 * @c bscope_all_band_fallback counter exists so the population is never silent.
 *
 * A SINGLE-band arc is never spared here: with one band there is no LOS anchor to
 * preserve (the (ρ, I, N) split is already unidentifiable from that arc alone),
 * and refusing a detector's positive slip evidence would leave the step standing
 * with no observable able to catch it again.  @ref BandScopeDecision::single_band
 * reports it so the run-end census can separate that population.
 *
 * @param live_bands    Every signal of this (station, satellite) with a live ambiguity.
 * @param implicated    The signals THIS detector named.
 * @param already_freed The signals of this arc an EARLIER drain already freed this
 *                      epoch (empty for the first drain).  They cannot be un-freed,
 *                      so they count toward covering the arc but are never chosen
 *                      as the survivor.
 * @param obs           The arc's raw row this epoch, or nullptr when none exists.
 * @param verdict       The arc's raw step adjudication, or nullptr.
 * @return The decision; @c scoped false means "do exactly what an unscoped run does".
 */
BandScopeDecision networkSlipBandScope(const std::set<std::string>& live_bands,
                                       const std::set<std::string>& implicated,
                                       const std::set<std::string>& already_freed,
                                       const RawSlipObs* obs,
                                       const RawSlipVerdict* verdict);

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
/**
 * @brief Outcome of the PASS-A MW refloat admission rule for ONE detector result.
 *
 * Factored out of the two admission sites so the rule is testable in isolation and
 * so both sites (the parallel per-station scan and the serial datum-station scan)
 * are provably the same rule.  Pure: no state, no logging, no allocation.
 */
struct MwAdmitDecision {
    /// The EVIDENCE rule with the knob factored out — bias-free lane, mature arc,
    /// finite ratio at or above the gate.  Evaluated ALWAYS so an OFF run can
    /// report its would-admit population (@ref would_admit).
    bool evidence_ok{ false };
    /// Today's gate, unchanged: known non-zero integer whose rounding residual is
    /// within the T0 repair tolerance.
    bool admit_legacy{ false };
    /// @ref evidence_ok actually in force (the knob is armed).
    bool admit_evidence{ false };
    /// The candidate is pushed.
    bool admitted{ false };
    /// Pushed, and ONLY the evidence rule put it there — the population whose
    /// event rows carry `src=evidence`.
    bool via_evidence{ false };
    /// DROPPED with a known integer whose residual was too loose.
    bool drop_loose{ false };
    /// DROPPED with no usable integer at all.
    bool drop_unknown{ false };
    /// Knob OFF, but the evidence rule alone would have admitted it.  This is the
    /// counter that sizes the card before anyone arms it.
    bool would_admit{ false };
};

/**
 * @brief The PASS-A MW refloat admission rule (pure).
 *
 * The legacy half re-uses the T0 integer-REPAIR gate on a DETECT-ONLY path whose
 * consumer never applies the integer, so it grades how cleanly a step ROUNDS and
 * not how far above the noise it STOOD.  The evidence half asks the second
 * question directly and is restricted to lanes where a strong flag means a slip:
 * BIA-corrected lanes are single-differenced against the detector's own MW pivot,
 * so a pivot S-transform re-reference presents there as a synchronous
 * multi-satellite step and is the measured false-alarm population.
 *
 * @param slip_wl_known   @c MwSlipResult::slip_wl_known.
 * @param slip_wl_cycles  @c MwSlipResult::slip_wl_cycles.
 * @param slip_frac       @c MwSlipResult::slip_frac (rounding residual, cycles).
 * @param frac_max        network.cycle_slip.turbo_edit_frac_max.
 * @param evidence_ratio  @c MwSlipResult::evidence_ratio (|dev|/threshold; NaN
 *                        where the emitting path published none — refused).
 * @param arc_epochs      @c MwSlipResult::arc_epochs (arc samples at the flag).
 * @param bia_corrected   @c MwSlipResult::bia_corrected (lane class).
 * @param knob_on         network.cycle_slip.mw_evidence_refloat.
 * @param ratio_min       network.cycle_slip.mw_evidence_ratio_min.
 * @param min_arc_epochs  network.cycle_slip.mw_evidence_min_arc_epochs.
 * @return The decision; @c admitted is what the caller acts on.
 */
inline MwAdmitDecision networkMwAdmitCandidate(bool slip_wl_known, int slip_wl_cycles,
                                               double slip_frac, double frac_max,
                                               double evidence_ratio, int arc_epochs,
                                               bool bia_corrected,
                                               bool knob_on, double ratio_min,
                                               int min_arc_epochs) {
    MwAdmitDecision d;
    d.admit_legacy = slip_wl_known && slip_wl_cycles != 0 && slip_frac <= frac_max;
    d.evidence_ok  = !bia_corrected
                     && arc_epochs >= min_arc_epochs
                     && std::isfinite(evidence_ratio)
                     && evidence_ratio >= ratio_min;
    d.admit_evidence = knob_on && d.evidence_ok;
    d.admitted       = d.admit_legacy || d.admit_evidence;
    d.via_evidence   = !d.admit_legacy && d.admit_evidence;
    d.would_admit    = !knob_on && !d.admit_legacy && d.evidence_ok;
    d.drop_loose     = !d.admitted && slip_wl_known && slip_frac > frac_max;
    d.drop_unknown   = !d.admitted && !slip_wl_known;
    return d;
}

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
        /// True when @ref dn_wl is a usable integer.  FALSE candidates exist only
        /// on the evidence path and must never reach the delta-bridge intercept:
        /// filling a pend's has_dn with a zero integer would BRIDGE the carried
        /// level by zero, i.e. assert the slip did not move it.
        bool   integer_known{ true };
        double evidence_ratio{ std::numeric_limits<double>::quiet_NaN() };  ///< |dev|/threshold at the flag.
        int    arc_epochs{ 0 };      ///< Arc samples at the flag.
        /// Admitted by the EVIDENCE rule alone (the legacy gate refused it) —
        /// drives the `src=evidence` event-row detail.
        bool   via_evidence{ false };
    };
    std::vector<std::vector<MwRefloatCand>> sta_mw_refloat;

    /// MW admission census, filled per station in the parallel region (index-
    /// addressed, no shared write) and reduced SERIALLY into
    /// NetworkServiceCounters.  Collected KNOB-INDEPENDENTLY: with
    /// mw_evidence_refloat off this is what the card WOULD have changed, which is
    /// the only honest way to size it before arming (invariant #4).
    struct MwAdmitCounts {
        long long drop_loose{ 0 };            ///< Known integer, residual too loose, refused.
        long long drop_unknown{ 0 };          ///< No usable integer, refused.
        long long evidence_admitted{ 0 };     ///< Admitted by the evidence rule alone (knob ON).
        long long evidence_would_admit{ 0 };  ///< Knob OFF; the evidence rule alone would have.
        long long legacy_admitted{ 0 };       ///< Admitted by today's integer gate.
    };
    std::vector<MwAdmitCounts> sta_mw_admit;

    /// COMMON-BAND SCOPE evidence (SPEC_event_harness §14): per station, the
    /// per-satellite MW silence / MW flags / LLI this epoch, filled in the
    /// parallel region (per-station slot, no shared write) and consumed by the
    /// SERIAL verdict seam before either drain.  Collected KNOB-INDEPENDENTLY —
    /// the census is the only honest way to size the card before arming it
    /// (invariant #4) — and it reads detector output that PASS A already has in
    /// hand, so the OFF path pays a map insert and nothing else.
    std::vector<std::map<std::string, CommonBandEvidence>> sta_cb_ev;

    explicit PassABuffers(std::size_t n)
        : a_ok(n, 0), a_err(n), sta_repairs(n), sta_if_resets(n), sta_mw_refloat(n),
          sta_mw_admit(n), sta_cb_ev(n) {}
};

/**
 * @brief COMMON-BAND SCOPE seam (SPEC_event_harness.md §14): form ONE verdict
 *        per (station, satellite) for this epoch, BEFORE any drain runs.
 *
 * Reads @c PassABuffers::sta_cb_ev (the parallel region's MW silence / MW flags
 * / LLI), previews the network-pooled IF-TD flag READ-ONLY so the equal-count
 * veto can be applied before the driver that decides it, applies the §14.2 rule
 * through @ref networkCommonBandVerdict, writes the `T0_band_scope` event rows
 * (armed only) and fills @p out's census.
 *
 * Runs whether or not the knob is armed: with it OFF the verdicts are counted
 * (`would_narrow` is what arming would change) but @c CommonBandEpoch::narrowed_at
 * stays empty and no event row is written, so the run is byte-identical.
 *
 * @param pa           PASS-A buffers (reads @c sta_cb_ev).
 * @param config_      Config (the scope's two knobs and the IF-TD threshold set).
 * @param stations     Station roster — read-only here: live ambiguity bands, the
 *                     IF-TD detectors' cached jumps and the per-pair EWMA scales.
 * @param present      Per-station PASS-A presence (absent stations are skipped).
 * @param i_lo         First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total  Station count.
 * @param se           Current synced epoch (event-row timestamp).
 * @param traces       Trace sink (the `T0_band_scope` rows).
 * @param log          Run logger.
 * @param anchor_incumbent  The election's sticky incumbent map
 *                     ("&lt;sat&gt;_&lt;signal&gt;" → station name), or nullptr.
 *                     Drives the §14.5 ANCHOR CLAUSE: a would-be narrowing at a
 *                     station that anchors ANY family of that satellite becomes
 *                     today's all-band response, so the satellite's families are
 *                     never left realized at different stations.
 * @param[out] out     Cleared and refilled; the drains read it through
 *                     @ref commonBandMayFree.
 */
void networkCommonBandVerdictEpoch(const PassABuffers& pa,
                                   const GnssConfig& config_,
                                   const std::vector<NetStation>& stations,
                                   const std::vector<bool>& present,
                                   int i_lo, int n_sta_total,
                                   const SyncedEpoch& se,
                                   NetworkRunTraces& traces,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   const std::map<std::string, std::string>* anchor_incumbent,
                                   CommonBandEpoch& out);

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
                                        LocalCarry>* bridge_local_carry = nullptr,
                               NetworkSlipOracle* oracle = nullptr,
                               /// §14 COMMON-BAND SCOPE: this epoch's verdicts.  Null
                               /// (the knob off) ⇒ @ref commonBandMayFree answers true
                               /// for every signal and the drain is byte-identical.
                               const CommonBandEpoch* common_band = nullptr);

/**
 * @brief SPEC_phase_windup_sign §10.4 — drain the accumulator-RESTART ledgers
 *        and refloat the stepped arcs (the wired response standing invariant
 *        #1 demands before any @c accumulate_phase_windup arm).
 *
 * A restart applies an exact integer-cycle step to EVERY signal of the
 * satellite (MW-blind: the step is band-common so ΔWL = 0) while the carried
 * ambiguity and the zero-q leveling level survive at their pre-gap values.
 * The response is the same free-epoch refloat the raw-slip drains use: mark
 * pending-reseed, latch @c free_ambs (cause @c kCauseWindup — the lane
 * observatory grades this response like any other), feed the C3c funnel under
 * carry_invariant.  Runs SERIAL, station-ordered, AFTER the per-station model
 * evaluations filled the ledgers and BEFORE the lane observatory's PASS-A
 * half (the freed set must be complete).  Each station's ledger is cleared
 * after the drain; entries stamped with an earlier epoch (the station was
 * absent when its restart fired) are REFUSED and counted — a response to
 * history would refloat an arc whose step was already absorbed at re-entry.
 * No-op unless @c network.cycle_slip.windup_restart_refloat (which the loader
 * disarms without accumulated mode — the verdict does not exist otherwise).
 */
void networkSlipDrainWindupRestart(const GnssConfig& config_,
                                   std::vector<NetStation>& stations,
                                   const std::vector<bool>& present,
                                   int i_lo, int n_sta_total,
                                   bool carry_invariant, bool carry_warm,
                                   std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                   SlipQcCounters& qc_cnt,
                                   const SyncedEpoch& se, NetworkRunTraces& traces,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   RelevelEpochState* relevel = nullptr,
                                   std::map<std::tuple<int, std::string, std::string>,
                                            RelevelPend>* relevel_pend = nullptr,
                                   NetworkSlipOracle* oracle = nullptr,
                                   const RawSlipEpoch* bscope_raw = nullptr);

/**
 * @brief Card G RESPONSE (docs/SPEC_slip_qc_hardening.md §G3/W1) — mark the
 *        arcs that re-entered across a tracking break SLIPPED, in the SAME
 *        epoch, so the PASS-B leveling inject sees a free arc instead of
 *        bridging a stale zero-q level across the break.
 *
 * ## The defect this closes
 *
 * An arc that re-appears within @c network.phase_leveling_max_gap_s (900 s on
 * the FLD63 chain; the loader default is 120 s) has its PRE-GAP leveling carry
 * re-injected at converged weight — no time-difference detector can run on a
 * return epoch (MW needs an adjacent predecessor, IF-TD needs three consecutive
 * epochs), so the break is never marked and the carry bridges it.  Measured on
 * `runs/fldf3_pl2` (12 h, 63 field stations, the converging leveling posture):
 * 364 of 453 resumptions returned with the raw MW/GF level SHIFTED, and 88 arcs
 * of those re-entered BRIDGED — Card G's own G3 rule ("nonzero ⇒ spec the
 * follow-up card that marks such arcs slipped at re-entry via the C3c funnel").
 * One of them, BSHM00ISR/G22 at TOW 271200, injected a ~1e-5 m² carry against a
 * phase that had moved ~1451 wide-lane cycles; the solve absorbed it along the
 * wet-mapping null line (ΔCLK/ΔZWD = −1.35, R² 0.9996) and that station's ZWD
 * went to −2446 m.  Standing invariant #1: a stiff zero-q quantity crossing a
 * jump with NO wired response does not degrade gracefully.
 *
 * ## The verdict (three gates, evaluated per RESUMPTION)
 *
 *  - `shifted`      — the raw MW/GF level moved across the break beyond the
 *                     empirical gate.  Always slipped.
 *  - LLI on return  — the receiver asserted loss of lock on either band of the
 *                     primary pair at the return epoch
 *                     (@c SlipQcParams::gap_reentry_lli, default ON).  This is
 *                     the receiver's own verdict about the break, and it is the
 *                     only evidence an UNMEASURABLE resumption can carry.
 *  - `!measurable`  — no pre-gap dual-band level existed, so nothing could be
 *                     checked (@c gap_reentry_unmeasurable, default OFF: absence
 *                     of evidence is not evidence of a slip).
 *
 * ## The response is the DETECTOR response, verbatim
 *
 * Both bands' ambiguities are treated exactly as a detected slip — C3c funnel
 * insert under @p carry_invariant; pending-reseed plus a @c free_ambs latch
 * (cause @c RelevelEpochState::kCauseGap, so the lane observatory grades this
 * response like any other) under a warm leveling carry.  Nothing is repaired
 * and no integer is applied: across a loss of lock the slip magnitude is
 * unbounded and unknowable, so re-entry is treated as code-anchored-fresh.
 *
 * ## ⚠ The key is SYNTHESISED, and the target is the CARRY, not the state
 *
 * (The wiring defect fixed 2026-09-09; read this before "simplifying" the drain
 * back into the shape of its siblings.)  The MW/LLI drain's @c refloat_signal
 * walks the station's LIVE ambiguity map and matches (satellite, signal),
 * because a detected slip happens on an arc that is BEING MEASURED this epoch.
 * A RE-ENTRY is the opposite case by definition: the arc left the model set, so
 * its ambiguity column was pruned with the gap and PASS B's measurement pre-pass
 * (@c MeasurementModel::ensureRequiredStates) re-creates it only AFTER every
 * PASS-A slip drain has run.  Mirroring the sibling drain therefore matched
 * nothing — measured on a 12 h / 63-station field arm: 396 re-entries judged
 * SLIPPED, 775 signals reported "no live ambiguity", ZERO arcs freed, and the
 * whole run came back BYTE-IDENTICAL to its OFF control.
 *
 * So the key is built with @c StateKeys::makeAmbiguityKey — the exact format the
 * PASS-B inject site reads (@c relevel.free_ambs.count({i, key}) over
 * @c StateManager::getAmbiguities() keys) and the exact format the Card-G arc
 * classification synthesises, so a freed arc reads @c shifted_fresh there by
 * construction — and the "is there anything to protect?" question is asked of
 * the CARRY: a @ref LocalCarry entry for (station, satellite, signal) whose
 * @c last_seen_s is still inside @p pl_max_gap_s is precisely what the inject
 * would fold across the break.  An arc with no such entry (or one already past
 * the window) is counted @c gap_reentry_no_carry and left alone: the inject
 * would not have bridged it either.  Under @p carry_invariant every re-entry is
 * treated as protectable — the DD chord test needs
 * @c NetworkInvariantCarryDriver, which is a @c runNetwork local and does not
 * reach this seam; the funnel insert is idempotent and an arc with no carried
 * chord simply re-taps fresh, so that is the conservative direction.
 *
 * Limitation, stated rather than discovered: under an IF-COMBO ambiguity basis
 * the live key is @c "<sat>_<sigA>_<sigB>" and this per-band key would not match
 * it.  The network producer is uncombined (per-band) and the Card-G
 * classification carries the identical exposure, so both instruments agree; a
 * combined basis has to widen BOTH sites together.
 *
 * Runs SERIAL, station-ordered → deterministic.  Sited in the slip-drain band
 * of PASS A (after the wind-up restart drain, before the lane observatory's
 * PASS-A half) so the freed set is complete for the observatory and visible to
 * the PASS-B inject in the SAME epoch — a response that landed one epoch late
 * would have let the stale level fold once, which is the whole defect.
 *
 * Stations below @p i_lo (the gauge-pinned legacy pivot) are SKIPPED and
 * counted: their ambiguities are pinned, so a local refloat is meaningless
 * there — the datum-station analogue is the MW drain's network re-reference.
 *
 * No-op unless @c SlipQcParams::gap_reentry.  Every counter it touches is in
 * @ref SlipQcCounters and closes (see the block comment there).
 *
 * @param stations             Station roster — @c sm is MUTATED (pending-reseed marks).
 * @param present              Per-station PASS-A presence (absent stations produce no resumptions anyway).
 * @param i_lo                 First non-pivot station index (0 under the prioritized datum).
 * @param n_sta_total          Station count.
 * @param carry_invariant      Card-4 DD invariant carry vehicle armed.
 * @param carry_warm           The legacy phase-leveling carry is warm (a stale value can
 *                             exist).  Implies @c network.phase_leveling by
 *                             construction (ApplicationNetwork.cpp: `carry_warm = pl_on
 *                             && ...`), which is why a null @p relevel under a warm carry
 *                             is unambiguously a MISSING free-epoch latch
 *                             (@c gap_reentry_no_relevel) and not "some other vehicle".
 * @param carry_slipped_ambs   C3c funnel — INSERTED into under @p carry_invariant.
 * @param raw                  THIS epoch's raw-observable evidence (reads @c resumptions).
 * @param qc                   Knob snapshot (the three @c gap_reentry_* fields).
 * @param local_carry          The leveling carry map, READ-ONLY — the thing a
 *                             re-entry actually threatens, and the drain's
 *                             "anything to protect?" oracle (see the banner above).
 * @param pl_now_s             This epoch's continuous-GPS seconds (the inject site's
 *                             @c pl_now_s, so the gap arithmetic is identical).
 * @param pl_max_gap_s         @c network.phase_leveling_max_gap_s — beyond it the
 *                             PASS-B inject skips the entry anyway (THE CLIFF), so
 *                             there is nothing for this response to prevent.
 * @param qc_cnt               Run-end counters — accumulated into.
 * @param relevel              Free-epoch latch, or nullptr when
 *                             @c cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend         Bridge-v2 pend map, or nullptr — a fresh mark voids
 *                             any outstanding pend on the same (station, satellite),
 *                             exactly as the raw-slip drains do.
 */
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
                                RelevelEpochState* relevel = nullptr,
                                std::map<std::tuple<int, std::string, std::string>,
                                         RelevelPend>* relevel_pend = nullptr);

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
                           const std::vector<RawSlipHistory>* raw_hist = nullptr,
                           NetworkSlipOracle* oracle = nullptr,
                           /// §14 COMMON-BAND SCOPE: this epoch's verdicts.  Null
                           /// (the knob off) ⇒ the pair refloat frees both bands.
                           const CommonBandEpoch* common_band = nullptr,
                           /// §14.10 PAIR WIDTH: run-end tally sink, per pair
                           /// KIND (evaluated / flagged / refloated).  Filled
                           /// knob-independently (invariant #4) — a `first` run
                           /// reports the one watched pair.  Null skips it.
                           IfTdPairTallies* pair_tally = nullptr);

/**
 * @brief EVENT HARNESS Study 1 - the injected @c free_epoch arc response.
 *
 * (docs/SPEC_event_harness.md SS9.)  The T4 attribution erase and a detector
 * refloat are two different answers to "this arc's level is not trusted", and
 * the harness measured them 11x apart on the same healthy arc.  This drain is
 * the DETECTOR half made injectable: an @c arc_erase event carrying
 * @c "mode": "free_epoch" is routed HERE instead of to the T4 block, and does
 * exactly what @c refloat_signal does for a detected slip - C3c funnel insert
 * under @p carry_invariant, pending-reseed mark + free-epoch latch + cause tag
 * under @p carry_warm, and the bridge-pend void that a fresh mark implies.
 * It does NOT erase the @c LocalCarry entry, does NOT co-erase the LOS iono
 * carry and does NOT queue the RAIM funnel, which is the whole contrast.
 *
 * Sited with the other PASS-A drains (and after them) so the free-epoch latch
 * it fills is read by the SAME PASS-B leveling inject in the SAME epoch.  The
 * T4 block could not host it: post-fit RAIM runs after that inject, so a
 * free_ambs insert there would be cleared before any site read it.
 *
 * Harness-only and unreachable unless @c network.inject staged a @c free_epoch
 * directive this epoch (@p arc_free empty on every other run) - byte-identical
 * by construction, invariant #6.
 *
 * @param arc_free           This epoch's staged directives (every mode; the
 *                           non-@c free_epoch ones are skipped here and taken
 *                           by the T4 block).
 * @param stations           Station roster - @c sm is MUTATED (pending-reseed).
 * @param present            Per-station PASS-A presence.
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry vehicle armed.
 * @param carry_warm         The legacy phase-leveling carry is warm.
 * @param carry_slipped_ambs C3c funnel - inserted into under @p carry_invariant.
 * @param inject             Injector ledger - @c n_applied / @c fired_by_kind /
 *                           @c arc_erase_mode_acted are closed here for the
 *                           directives this drain consumes (the never-fired
 *                           hard error is the other consumer's otherwise).
 * @param se                 This epoch (event rows).
 * @param traces             Trace bundle - one INJECT row per acted arc.
 * @param log                Logger.
 * @param relevel            Free-epoch latch, or nullptr when
 *                           @c cycle_slip.relevel_free_epoch is off (then the
 *                           mark alone cannot deliver the free epoch and the
 *                           event row says so).
 * @param relevel_pend       Bridge-v2 pend map, or nullptr.
 */
void networkSlipDrainInjectedFree(const std::vector<InjectArcErase>& arc_free,
                                  std::vector<NetStation>& stations,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  bool carry_invariant, bool carry_warm,
                                  std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                  NetworkInjectState& inject,
                                  const SyncedEpoch& se, NetworkRunTraces& traces,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  RelevelEpochState* relevel = nullptr,
                                  std::map<std::tuple<int, std::string, std::string>,
                                           RelevelPend>* relevel_pend = nullptr);

} // namespace GnssProcessor

#pragma once
/**
 * @file NetworkSlipQc.hpp
 * @brief Network run-loop slip QC: the PASS-A buffers, the common-band verdict
 *        seam, the serial drains (phase-leveling T0 repair, MW-detect / LLI
 *        refloat, wind-up restart, row-level and per-signal gap re-entry,
 *        injected arc-free), the IF-phase triple-difference driver, the
 *        delta-bridge kernels, and their pure helpers.
 *
 * ifElevMap and parseIfPairResetKey moved here at decomposition INC-2, and the
 * serial drains of the SB_SLIP block moved here from ApplicationNetwork.cpp at
 * INC-6 (see docs/network_decomposition_plan.md).  The PASS-A parallel scan that
 * fills PassABuffers, and the calls into the drains, live in
 * Application::networkPassASlipScan_ (ApplicationNetworkAssembly.cpp, INC-7);
 * the SB_SLIP D0b timer bracket in ApplicationNetwork.cpp wraps that call.
 *
 * Threading: the drains, the common-band verdict seam and the IF-TD driver run
 * serially in station order, so their results are deterministic.  The parallel
 * scan fills PassABuffers, one index-addressed slot per station, and the pure
 * helpers below are safe to call from it.  Cross-epoch slip state stays with its
 * owners: detector internals and the per-(station, pair) IF EWMA scale
 * (NetStation::if_pair_scale) persist in NetStation, and the phase-leveling carry
 * map persists in NetworkCarryState.  The functions declared here own no state.
 */

#include "common/Constants.hpp"
#include "filter/NetworkRunTypes.hpp"   // NetStation, LocalCarry, TurboEditResolver::SignalRepair, PppObservation (transitive)
#include "filter/ArcLedger.hpp"        // ArcReentryEpoch / ArcReentryOutcomeMap (card SID phase 3)
#include "filter/StateKeySchema.hpp"   // StateKeys::parseAmbiguityKey (parseIfPairResetKey)
#include "filter/NetworkInject.hpp"     // InjectArcErase / NetworkInjectState (event harness, Study 1)
#include "logging/NetworkRunTraces.hpp" // NetworkRunTraces (emitEvent, if_slip_trace), FlagRate
#include "slip/NetworkRawObservables.hpp" // RawSlipEpoch/History, SlipQcParams/Counters (Cards E/L/G)
#include "slip/NetworkSlipOracle.hpp"   // NetworkSlipOracle (sim-only oracle slip response)
#include "slip/NetworkCommonBandScope.hpp"  // Common-band verdict types for the drain hook (SPEC_event_harness §14)

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
struct RosterN3bCensus;   // app/SignalRoster.hpp (card NOM step N3b)

/// Elevation mapping for the IF-phase slip stochastic model: the multiplicative
/// factor by which between-epoch double-differenced carrier-phase noise grows
/// toward the horizon.  A residual at elevation @p el_deg is normalized to a
/// zenith equivalent by dividing by this factor (that is, multiplying by sin el);
/// when thresholding, the empirical scale (pooled or per-station EWMA) is
/// multiplied by it per satellite, so a single scale holds across the whole sky.
/// 1/sin(el) is the standard first-order phase-noise shape.  The elevation is
/// clamped at 5° so the factor stays bounded near the horizon (which the elevation
/// mask already excludes).  A fitted a + b/sin(el) is a possible refinement.
inline double ifElevMap(double el_deg) {
    constexpr double kMinElDeg = 5.0;
    const double el_rad = std::max(el_deg, kMinElDeg) * (Constants::PI / 180.0);
    const double s = std::sin(el_rad);
    return (s > 1e-3) ? (1.0 / s) : 1.0;
}

/// Pooled centre and scale of one IF-TD signal pair, over that pair's samples
/// from every present station (steps 1–2 of @ref networkSlipIfTdDriver).
struct IfTdPairPool {
    double med{ 0.0 };     ///< Median of the raw double differences (m): the network common mode.
    double s_pool{ 0.0 };  ///< 1.4826·median(|z|), z = (dd−med)/ifElevMap(el), in zenith-equivalent m.
};

/// @brief Pooled IF-TD centre and robust scale for one signal pair.
///
/// Shared by @ref networkSlipIfTdDriver and the common-band scope's read-only
/// flag preview (SPEC_event_harness §14), so the preview cannot drift from the
/// decision it previews: the common-band verdict is formed before the drains,
/// while the pooled flag is decided inside the IF-TD driver, which runs after them.
///
/// The centre is removed in raw space because the network common mode (frame /
/// datum wobble) does not depend on elevation.  The scale excludes the
/// constellation reference satellite, whose double difference is identically
/// zero by construction and deflates the MAD by about 16 % (§6.14.5 defect 2).
/// When excluding it would leave no sample, the full set is used, so the median
/// is never taken over an empty set.  Both order statistics are (n/2)-th
/// elements, so the result does not depend on the order the samples arrive in.
///
/// @param dd      Raw between-epoch double differences, one per sample (m).
/// @param el      Satellite elevation of each sample (degrees); same length as @p dd.
/// @param is_ref  Non-zero where the sample is the constellation reference; same length.
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
/// Step 3 of @ref networkSlipIfTdDriver, factored out for the same reason as
/// @ref ifTdPoolStats.  The effective scale shrinks from the network pool prior
/// toward the station-pair's own EWMA as that pair accumulates clean samples, so
/// a quiet reference station and a noisy modernized-signal station each get
/// their own envelope instead of one threshold set by the majority.
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

/// Parse an IfPhaseSlipDetector reset key ("G01_L1C_L2W") into the satellite and
/// the two band signals of the IF pair it was formed from.  Returns false on a
/// malformed key.  The network carries per-band ambiguities and the IF combination
/// cannot localize which band slipped, so a flagged IF pair implicates both bands.
inline bool parseIfPairResetKey(const std::string& key, std::string& sat,
                                std::string& sig1, std::string& sig2) {
    // The key is a PPP ambiguity key whose signal is the pair text "L1C_L2W".
    const auto parts = StateKeys::parseAmbiguityKey(key);
    if (!parts) return false;
    const std::string& pair = parts->signal;
    const auto us = pair.find('_');
    if (us == std::string::npos || us == 0 || us + 1 >= pair.size()) return false;
    sat  = parts->satellite;
    sig1 = pair.substr(0, us);
    sig2 = pair.substr(us + 1);
    return !sat.empty() && !sig1.empty() && !sig2.empty();
}

/**
 * @brief Outcome of the PASS-A MW refloat admission rule for one detector result.
 *
 * Both admission sites (the parallel per-station scan and the serial
 * datum-station scan) call @ref networkMwAdmitCandidate, so they apply the same
 * rule, and the rule can be unit-tested alone.  Pure: no state, no logging, no
 * allocation.
 */
struct MwAdmitDecision {
    /// The evidence rule with the knob factored out: bias-free lane, mature arc,
    /// finite ratio at or above the gate.  Always evaluated, so a run with the knob
    /// off can report its would-admit population (@ref would_admit).
    bool evidence_ok{ false };
    /// The legacy gate: a known non-zero integer whose rounding residual is within
    /// the T0 repair tolerance.
    bool admit_legacy{ false };
    /// @ref evidence_ok in force (the knob is armed).
    bool admit_evidence{ false };
    /// The candidate is pushed.
    bool admitted{ false };
    /// Pushed, and only the evidence rule put it there: the population whose event
    /// rows carry `src=evidence`.
    bool via_evidence{ false };
    /// Dropped with a known integer whose residual was too loose.
    bool drop_loose{ false };
    /// Dropped with no usable integer at all.
    bool drop_unknown{ false };
    /// Knob off, but the evidence rule alone would have admitted it.  This count
    /// sizes the evidence rule's effect before the knob is armed.
    bool would_admit{ false };
};

/**
 * @brief The PASS-A MW refloat admission rule (pure).
 *
 * The legacy half reuses the T0 integer-repair gate on a detect-only path whose
 * consumer never applies the integer, so it grades how cleanly a step rounds, not
 * how far the step stands above the noise.  The evidence half tests the second
 * property directly, and only on lanes where a strong flag means a slip:
 * BIA-corrected lanes are single-differenced against the detector's own MW pivot,
 * so a pivot S-transform re-reference appears there as a synchronous
 * multi-satellite step, and those steps are the measured false-alarm population.
 *
 * @param slip_wl_known   @c MwSlipResult::slip_wl_known.
 * @param slip_wl_cycles  @c MwSlipResult::slip_wl_cycles.
 * @param slip_frac       @c MwSlipResult::slip_frac (rounding residual, cycles).
 * @param frac_max        network.cycle_slip.turbo_edit_frac_max.
 * @param evidence_ratio  @c MwSlipResult::evidence_ratio (|dev|/threshold; NaN
 *                        where the emitting path published none, which is refused).
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

/**
 * @brief PASS-A slip-scan buffers: filled by the parallel detection loop (one
 *        index-addressed slot per station, so no slot is written by two
 *        threads) and consumed by the serial drains below in station order.
 *
 * The constructor sizes every buffer to the station count (stations.size()) and
 * zero-fills a_ok.  Construct a new instance every epoch; do not hoist it for
 * reuse, since the scan appends to several slots without clearing them.
 */
struct PassABuffers {
    /// Per-station PASS-A geometry success flag, folded into present[] in a
    /// serial pass.  char, not bool: std::vector<bool> packs bits, so
    /// concurrent writes to adjacent indices would race.
    std::vector<char>        a_ok;
    /// Per-station PASS-A exception text.  An exception must not escape an OpenMP
    /// structured block, so each iteration catches its own and the text is logged
    /// serially.
    std::vector<std::string> a_err;
    /// Phase-leveling T0: per-station integer slip repairs, produced in the
    /// parallel loop into a private slot and applied to the shared carry map in
    /// the serial, station-ordered drain (networkSlipDrainT0Repairs).
    std::vector<std::vector<TurboEditResolver::SignalRepair>> sta_repairs;
    /// IF-phase slip detector output: per-station flagged IF-pair reset keys
    /// ("G01_L1C_L2W"), consumed in a serial, station-ordered marking pass
    /// (networkSlipIfTdDriver, reference-satellite common-mode fold).
    std::vector<std::vector<std::string>> sta_if_resets;
    /// MW detector (mw_detect) output: wide-lane slips to refloat, consumed in the
    /// serial MW-detect drain (networkSlipDrainMwRefloat).  Empty when turbo_edit
    /// runs the integer-repair path instead, or when mw_detect is off.  A detector
    /// result is pushed when @ref networkMwAdmitCandidate admits it: the legacy
    /// integer gate (slip_wl_known, non-zero, frac <= frac_max), or the evidence
    /// rule with network.cycle_slip.mw_evidence_refloat armed.  Each candidate
    /// carries the WL integer, its fractional confidence and the WL wavelength so
    /// the delta bridge can use them (SPEC_slip_relevel_walk §6).  Never fill it
    /// from slip_wl_float (candidate-selector-only contract, MwSlipDetector.hpp).
    struct MwRefloatCand {
        std::string sat, sig1, sig2;
        int    dn_wl{0};        ///< Confident ZD wide-lane integer (cycles).
        double frac{0.0};       ///< MW fractional distance (cycles): the confidence.
        double lambda_wl{0.0};  ///< Wide-lane wavelength (m).
        /// True when @ref dn_wl is a usable integer.  False only for candidates
        /// admitted on the evidence path, which must not reach the delta-bridge
        /// intercept: setting a pend's has_dn from a zero integer would bridge the
        /// carried level by zero, which asserts that the slip did not move it.
        bool   integer_known{ true };
        double evidence_ratio{ std::numeric_limits<double>::quiet_NaN() };  ///< |dev|/threshold at the flag.
        int    arc_epochs{ 0 };      ///< Arc samples at the flag.
        /// Admitted by the evidence rule alone (the legacy gate refused it); drives
        /// the `src=evidence` event-row detail.
        bool   via_evidence{ false };
    };
    std::vector<std::vector<MwRefloatCand>> sta_mw_refloat;

    /// MW admission census, filled per station in the parallel region
    /// (index-addressed, no shared write; with datum_station_detect armed, the
    /// serial datum-station scan fills station 0's slot) and reduced serially
    /// into NetworkServiceCounters.
    /// Collected whatever the knob state: with mw_evidence_refloat off it counts
    /// what arming the knob would change, which sizes that change before it is
    /// armed (invariant #4).
    struct MwAdmitCounts {
        long long drop_loose{ 0 };            ///< Known integer, residual too loose, refused.
        long long drop_unknown{ 0 };          ///< No usable integer, refused.
        /// The satellite of each @ref drop_unknown verdict, in detector order (one entry
        /// per refused pair).  The identity the producer AR search's suspect-corner mask
        /// stamps (card ARL, filter/NetworkArSuspect.hpp); filled only while that mask is
        /// armed, and read nowhere else.
        std::vector<std::string> drop_unknown_sats;
        long long evidence_admitted{ 0 };     ///< Admitted by the evidence rule alone (knob on).
        long long evidence_would_admit{ 0 };  ///< Knob off; the evidence rule alone would have admitted it.
        long long legacy_admitted{ 0 };       ///< Admitted by the legacy integer gate.
        /// Detector pairs the network did not form under card L5P
        /// (`network.cycle_slip.mw_detect_pairs: primary`), because the pair
        /// carries a band outside the constellation's ionosphere-free pin pair.
        /// Zero under the default `all` and on a two-band lineage, so a nonzero
        /// count is the on-path evidence that the scope acted (standing
        /// invariant #4).
        long long scope_skipped{ 0 };
    };
    std::vector<MwAdmitCounts> sta_mw_admit;

    /// Common-band scope evidence (SPEC_event_harness §14): per station, the
    /// per-satellite MW silence, MW flags and LLI of this epoch, filled in the
    /// parallel region (per-station slot, no shared write) and consumed by the
    /// serial verdict seam before either drain.  Collected whatever the knob
    /// state, so the census sizes the card before it is armed (invariant #4).  It
    /// reads detector output PASS A already holds, so with the knob off the cost
    /// is a map insert.
    std::vector<std::map<std::string, CommonBandEvidence>> sta_cb_ev;

    explicit PassABuffers(std::size_t n)
        : a_ok(n, 0), a_err(n), sta_repairs(n), sta_if_resets(n), sta_mw_refloat(n),
          sta_mw_admit(n), sta_cb_ev(n) {}
};

/**
 * @brief Common-band scope seam (SPEC_event_harness.md §14): form one verdict per
 *        (station, satellite) for this epoch, before any drain runs.
 *
 * Reads @c PassABuffers::sta_cb_ev (the parallel region's MW silence, MW flags
 * and LLI), previews the network-pooled IF-TD flag read-only so the equal-count
 * veto can be applied before the driver that decides it runs, applies the §14.2
 * rule through @ref networkCommonBandVerdict (and the §14.12 LLI-only rule
 * through @ref networkCommonBandLliVerdict), writes the `T0_band_scope` event
 * rows (armed only) and fills the census in @p out.
 *
 * Runs whether or not the knob is armed.  With it off the verdicts are counted
 * (`would_narrow` is what arming would change), but
 * @c CommonBandEpoch::narrowed_at stays empty and no event row is written, so the
 * run is byte-identical.
 *
 * @param pa           PASS-A buffers (reads @c sta_cb_ev).
 * @param config_      Config (the common-band scope knobs and the IF-TD threshold set).
 * @param stations     Station roster, read-only here: live ambiguity bands, the
 *                     IF-TD detectors' cached jumps and the per-pair EWMA scales.
 * @param present      Per-station PASS-A presence (absent stations are skipped).
 * @param i_lo         First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total  Station count.
 * @param se           Current synced epoch (event-row timestamp).
 * @param traces       Trace sink (the `T0_band_scope` rows).
 * @param log          Run logger.
 * @param anchor_incumbent  The election's sticky incumbent map
 *                     ("&lt;sat&gt;_&lt;signal&gt;" → station name), or nullptr.
 *                     Drives the §14.5 anchor clause: a narrowing that would apply
 *                     at a station anchoring any family of that satellite becomes
 *                     the all-band response instead, so the satellite's families
 *                     are not left realized at different stations.
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
 *        repairs to the phase-leveling carry before PASS B reads the carry to
 *        build the prior.
 *
 * Policy for the BIA-less network, chosen on data: a confident integer of sane
 * magnitude (within the TurboEdit frac and |dN| gates) is repaired by shifting
 * the carried value continuously (variance unchanged, covariance preserved); a
 * low-confidence detection is left untouched, because the float wide-lane's
 * low-confidence repairs are mostly false positives and acting on them measurably
 * degrades the products.  The station loop starts at @p i_lo: on the legacy datum
 * (i_lo = 1) the pivot stations[0] is not touched, and under the prioritized
 * datum (i_lo = 0) station 0 is processed like any other.  The full rationale is
 * in the comment at the top of the function body in NetworkSlipQc.cpp.
 *
 * @param pa           PASS-A buffers (reads sta_repairs).
 * @param config_      Config (TurboEdit frac/dN acceptance gates read inside).
 * @param stations     Station roster (names for logging only).
 * @param present      Per-station PASS-A presence (absent stations skipped).
 * @param i_lo         First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total  Station count.
 * @param local_carry  Phase-leveling carry map, modified in place (value_abs
 *                     shifted by dN·λ on each accepted repair).  Pass the
 *                     ctx.carry.local_carry instance itself, not a copy.
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
 * @brief MW-detect / LLI refloat drain: refloat the ambiguities flagged by an
 *        admitted PASS-A wide-lane candidate or by the receiver's own LLI
 *        loss-of-lock bit.  No integer is applied, so a wrong wide-lane integer
 *        cannot bias the gauge.
 *
 * Gated inside on network.cycle_slip.mw_detect; call it unconditionally.  The
 * refloat_signal lambda lives in this drain and is deliberately kept separate
 * from the IF driver's refloat_pair.  The drain also feeds the Card-4 C3c
 * funnel: every flagged (station, ambiguity) is inserted into
 * @p carry_slipped_ambs from epoch 1, independent of the legacy carry's warm gate.
 *
 * ### Detections are emitted when the carry is cold (card E)
 *
 * refloat_signal reports matched and refloated separately.  Event emission and
 * the n_mw / n_lli log counters key on the detection; refloat accounting is a
 * separate, warm-only counter.  A cold run therefore writes `mw_wl_slip` /
 * `lli_slip` rows with `action=diagnostic`, as the IF-TD driver does, and still
 * feeds the C3c funnel.  The split is numerically inert:
 * markAmbiguityPendingReseed and the @p carry_slipped_ambs insert run under the
 * same conditions as before it.
 *
 * ### The LLI arm is qualified, not excluded (card L)
 *
 * An LLI assertion acts when an empirical MW or GF step corroborates it
 * (decisive at any station, so a real multi-satellite re-lock is always acted
 * on), or when the station's own LLI behaviour is still credible (a two-state,
 * hysteretic EWMA of its LLI-set fraction).  Only the remaining case, an
 * uncorroborated assertion at a station whose LLI is not credible, is
 * downgraded: it is emitted with `action=suppressed` and counted, not dropped
 * silently.  The whole refloat_signal call is skipped for a suppressed
 * assertion, so the C3c funnel insert follows the same qualification as the
 * pending-reseed path (spec M5).  The qualification is inert unless
 * @c SlipQcParams::lli_corroborate is armed.
 *
 * @param pa                 PASS-A buffers (reads sta_mw_refloat).
 * @param config_            Config (mw_detect gate read inside).
 * @param stations           Station roster (StateManager pending-reseed marks).
 * @param present            Per-station PASS-A presence.
 * @param modeled            Per-station modeled observations (LLI flag scan).
 * @param i_lo               First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry armed (C3c feed gate).
 * @param carry_warm         Legacy phase-leveling carry past its warmup gate
 *                           (refloat is meaningful only when warm; cold, the
 *                           detection is diagnostic).
 * @param carry_slipped_ambs The per-epoch C3c slip-event set: the single instance
 *                           owned by the run loop, by non-const reference.  A value
 *                           copy anywhere on this path would silently disable the
 *                           Card-4 slip funnel (standing invariant #1).
 * @param raw                This epoch's shared raw observables and step verdicts
 *                           (card L evidence).  With @c computed false there is no
 *                           evidence: the qualification is skipped and LLI stays
 *                           decisive.
 * @param qc                 Card L / G knob snapshot (all off: legacy behaviour).
 * @param raw_hist           Per-station cross-epoch slip-QC state; the per-station
 *                           LLI accounting for the M4 top-offenders line is
 *                           tallied here.
 * @param qc_cnt             Run-end slip-QC counters, accumulated into.
 * @param se                 Current synced epoch (event rows / log lines).
 * @param traces             Trace unit (T0_mw_detect event rows).
 * @param log                Run logger.
 *
 * ### Datum-station slip detect (network.cycle_slip.datum_station_detect)
 *
 * The four datum_* parameters arm the §9.7.10 datum-station path.  The caller
 * passes them together, and only under the legacy pivot datum with the toggle
 * armed; with the three queues null the path is off and the drain is
 * byte-identical.  Armed, the station loop starts at station 0, and a station-0
 * MW candidate or qualified LLI assertion takes the datum re-reference path
 * instead of refloat_signal.  The satellite is queued in @p datum_pb_reref_sats,
 * which this epoch's stash consumes (a hard skip of the PB carry), and in
 * @p datum_amb_reref_sats, which the first drain after that skip consumes: a
 * block ahead of this drain's station loop pending-reseeds the satellite's
 * ambiguities at every non-pivot station, erases their local carry, feeds the
 * Card-4 C3c funnel, inserts the satellite into @p datum_mark_sats for a
 * per-satellite disc mark at the next emit, and emits the `datum_reref` event.
 *
 * @param datum_pb_reref_sats  Per-satellite PhaseBias skip-stash queue
 *                             (net::SatelliteState::datum_reref_pb, copied in); nullptr: off.
 * @param datum_amb_reref_sats Per-satellite deferred all-station ambiguity reseed
 *                             queue (net::SatelliteState::datum_reref_amb, copied in).
 * @param datum_mark_sats      Per-satellite disc-mark queue at the next emit
 *                             (ctx.carry.refsat_mark_sats, the Edge-1 piece-2
 *                             mark-and-serialize path).
 * @param datum_local_carry    Phase-leveling carry map (the ctx.carry.local_carry
 *                             instance) for the local-carry erase on
 *                             re-reference, counted in datum_slip_carry_erases.
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
                               /// Common-band scope verdicts for this epoch (§14).
                               /// Null (the knob off): @ref commonBandMayFree answers
                               /// true for every signal and the drain is byte-identical.
                               const CommonBandEpoch* common_band = nullptr);

/**
 * @brief The arcs a wind-up accumulator restart of (station @p i, satellite
 *        @p sat) steps, as (ambiguity key, signal) pairs (card WUP).
 *
 * The satellite's live ambiguity columns in @p sm, in the map's order, when it
 * has any.  Otherwise, a re-entering arc whose column was pruned with the gap:
 * the (station, satellite)'s leveling carry entries in @p local_carry whose age
 * (@p pl_now_s minus @c last_seen_s) lies in [0, @p pl_max_gap_s], on the
 * synthesised key (@c StateKeys::makeAmbiguityKey) the PASS-B inject reads.
 * Empty when neither exists, which is the restart with nothing to free.
 *
 * @param sm            The station's state manager (read-only).
 * @param i             App station index (the carry map's first key).
 * @param sat           Satellite id.
 * @param local_carry   The leveling carry map; null reaches live columns only.
 * @param pl_now_s      This epoch's continuous GPS seconds (the carry's clock).
 * @param pl_max_gap_s  @c network.phase_leveling_max_gap_s.
 * @param via_carry     Set to true when the targets came from the carry (may be null).
 * @param arc_ledger    The run's arc ledger, or null.  Given, the carry targets are named
 *                      by the ledger (card NOM step N3b): the carry entries of the
 *                      (station, satellite) inside the horizon, in signal order, whose
 *                      ledger record is admitted and not dead.  A carried arc the ledger
 *                      holds no admission for (one admitted before a warm resume, which
 *                      restores the carry and not the ledger) is named by the carry, as
 *                      the census's fallback.  The carry walk is still formed and compared.
 * @param n3b           The N3b census (site @c windup_restart_arcs), or null.
 * @return The targets, live columns first-and-only when any exist.
 */
std::vector<std::pair<std::string, std::string>> networkWindupRestartTargets(
        const StateManager& sm, int i, const std::string& sat,
        const std::map<std::tuple<int, std::string, std::string>, LocalCarry>* local_carry,
        double pl_now_s, double pl_max_gap_s, bool* via_carry = nullptr,
        const ArcLedger* arc_ledger = nullptr, RosterN3bCensus* n3b = nullptr);

/**
 * @brief Drain the wind-up accumulator-restart ledgers and refloat the stepped
 *        arcs (SPEC_phase_windup_sign §10.4): the wired response that standing
 *        invariant #1 requires before any @c accumulate_phase_windup arm.
 *
 * A restart applies an exact integer-cycle step to every signal of the satellite
 * (MW-blind: the step is band-common, so ΔWL = 0), while the carried ambiguity
 * and the zero-q leveling level survive at their pre-gap values.  The response
 * is the free-epoch refloat the raw-slip drains use: under a warm carry, mark
 * pending-reseed, latch @c free_ambs (cause @c kCauseWindup) and void the
 * outstanding bridge pends of the (station, satellite); under carry_invariant,
 * feed the C3c funnel.
 *
 * Runs serially in station order, after the per-station model evaluations have
 * filled the ledgers, so the freed set is complete for the PASS-B inject.  Each
 * present station's ledger is cleared after the
 * drain; an absent station is skipped and keeps its ledger.  An entry stamped
 * with an earlier epoch (one that no drain consumed at the time) is refused and
 * counted (@c wu_restart_stale): its step has been absorbed since, and a
 * response now would refloat a clean arc.  An entry whose gap exceeds
 * @c network.phase_leveling_max_gap_s is counted (@c wu_restart_beyond_carry)
 * and not refloated, since the leveling carry was pruned with the gap.
 *
 * No-op unless @c network.cycle_slip.windup_restart_refloat.  The loader
 * disarms that knob without @c estimation.accumulate_phase_windup (the restart
 * verdict exists only in accumulated mode) and without @c network.phase_leveling.
 *
 * ## The re-entering arc (card WUP)
 *
 * A restart is a return after a gap longer than the wind-up horizon, so the arc
 * has usually left the model set: its ambiguity column was pruned with the gap and
 * PASS B re-creates it only after this drain.  The live columns of the satellite
 * are the target when they exist; otherwise the target is the (station, satellite)'s
 * leveling carry entries inside the carry horizon (@p local_carry, @p pl_now_s,
 * @p pl_max_gap_s), freed on the synthesised key the PASS-B inject reads, as
 * @ref networkSlipDrainGapReentry does.  Only a live column takes the
 * pending-reseed mark.  Counted: @c wu_restart_carry_arcs (arcs reached through the
 * carry) and @c wu_restart_no_arc (a warm, in-horizon restart with nothing to
 * free), which is outside @c wu_restart_refloated.  Without @p local_carry the
 * drain reaches live columns only, the behaviour before the card.
 *
 * @param local_carry   The leveling carry map, read-only; null reaches live columns only.
 * @param pl_now_s      This epoch's continuous GPS seconds (the carry's clock).
 * @param pl_max_gap_s  @c network.phase_leveling_max_gap_s.
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
                                   const std::map<std::tuple<int, std::string, std::string>,
                                                  LocalCarry>* local_carry = nullptr,
                                   double pl_now_s = 0.0, double pl_max_gap_s = 0.0,
                                   const ArcLedger* arc_ledger = nullptr,
                                   RosterN3bCensus* n3b = nullptr);

/**
 * @brief Gap re-entry response: mark the arcs that re-entered across a tracking
 *        break slipped, in the same epoch, so the PASS-B leveling inject sees a
 *        free arc instead of bridging a stale zero-q level across the break
 *        (card G, docs/SPEC_slip_qc_hardening.md §G3/W1).
 *
 * ## Why
 *
 * An arc that re-appears within @c network.phase_leveling_max_gap_s (loader
 * default 120 s) has its pre-gap leveling carry re-injected at converged weight.
 * No time-difference detector can run on a return epoch (MW needs an adjacent
 * predecessor, IF-TD three consecutive epochs), so without this drain the break
 * is never marked and the carry bridges it.  A stiff zero-q level carried across
 * such a jump does not degrade gracefully: the solve absorbs the tension
 * elsewhere (standing invariant #1; the measured field cases are in §G3/W1).
 *
 * ## The verdict (three gates, evaluated per resumption)
 *
 *  - `shifted`      the raw MW/GF level moved across the break beyond the
 *                   empirical gate.  Always slipped.
 *  - LLI on return  the receiver asserted loss of lock on either band of the
 *                   primary pair at the return epoch
 *                   (@c SlipQcParams::gap_reentry_lli, default on).  This is the
 *                   receiver's own verdict about the break, and it is the only
 *                   evidence an unmeasurable resumption can carry.
 *  - `!measurable`  no pre-gap dual-band level existed, so nothing could be
 *                   checked (@c gap_reentry_unmeasurable, default off, since
 *                   missing evidence is not evidence of a slip).
 *
 * ## The response is the detector response
 *
 * Both bands' ambiguities (and, under `processing.channel_mode: all`, the
 * station's other carried phases of those two bands) are treated as a detected
 * slip: the C3c funnel insert under @p carry_invariant; the pending-reseed mark
 * (where the ambiguity column already exists) and a @c free_ambs latch (cause
 * @c RelevelEpochState::kCauseGap) under a warm leveling carry.  Nothing is
 * repaired and no
 * integer is applied: across a loss of lock the slip magnitude is unbounded and
 * unknown, so the re-entry is treated as a fresh, code-anchored arc.
 *
 * ## The key is synthesized, and the target is the carry, not the state
 *
 * Do not rewrite this drain in the shape of its siblings.  The MW/LLI drain's
 * @c refloat_signal walks the station's live ambiguity map and matches
 * (satellite, signal), because a detected slip happens on an arc that is being
 * measured this epoch.  A re-entry is the opposite case: the arc left the model
 * set, so its ambiguity column was pruned with the gap, and PASS B's measurement
 * pre-pass (@c MeasurementModel::ensureRequiredStates) re-creates it only after
 * every PASS-A slip drain has run.  A drain that walked the live map would match
 * nothing and free no arc.
 *
 * The key is therefore built with @c StateKeys::makeAmbiguityKey, the format the
 * PASS-B inject site reads (@c relevel.free_ambs.count({i, key}) over
 * @c StateManager::getAmbiguities() keys) and the format the Card-G arc
 * classification synthesizes, so a freed arc reads @c shifted_fresh there by
 * construction.  The question "is there anything to protect?" is asked of the
 * carry: a @ref LocalCarry entry for (station, satellite, signal) whose
 * @c last_seen_s is still inside @p pl_max_gap_s is what the inject would fold
 * across the break.  An arc with no such entry (or one already past the window)
 * is counted @c gap_reentry_no_carry and left alone, since the inject would not
 * bridge it either.  Under @p carry_invariant every re-entry is treated as
 * protectable: the DD chord test needs @c NetworkInvariantCarryDriver, a
 * @c runNetwork local that does not reach this seam.  That is the conservative
 * direction, because the funnel insert is idempotent and an arc with no carried
 * chord simply re-taps fresh.
 *
 * Limitation: under an IF-combination ambiguity basis the live key is
 * @c "<sat>_<sigA>_<sigB>", which this per-band key does not match.  The network
 * producer is uncombined (per-band), and the Card-G classification has the same
 * exposure, so the two instruments agree; a combined basis would need both sites
 * widened together.
 *
 * Runs serially in station order (deterministic).  Called in the slip-drain band
 * of PASS A, after the wind-up restart drain, so the freed set is visible to the
 * PASS-B inject in the same epoch; a response one epoch late would let the stale
 * level fold once.
 *
 * On the legacy datum (i_lo = 1), resumptions at station 0, the gauge-pinned
 * pivot, are skipped and counted (pivot_skipped): its ambiguities are pinned, so
 * a local refloat has no meaning there; the datum-station counterpart is the MW
 * drain's network re-reference.  Under the prioritized datum (i_lo = 0) no
 * station is skipped.
 *
 * No-op unless @c SlipQcParams::gap_reentry.  Every counter it updates is in
 * @ref SlipQcCounters, and the counters partition their population (see the
 * block comment there).
 *
 * @param stations             Station roster; @c sm is modified (pending-reseed marks).
 * @param present              Per-station PASS-A presence (absent stations produce no resumptions anyway).
 * @param i_lo                 First non-pivot station index (0 under the prioritized datum).
 * @param n_sta_total          Station count.
 * @param carry_invariant      Card-4 DD invariant carry vehicle armed.
 * @param carry_warm           The legacy phase-leveling carry is warm (a stale value can
 *                             exist).  Implies @c network.phase_leveling by
 *                             construction (ApplicationNetwork.cpp: `carry_warm = pl_on
 *                             && ...`), which is why a null @p relevel under a warm carry
 *                             is counted as a missing free-epoch latch
 *                             (@c gap_reentry_no_relevel), except under
 *                             @p carry_invariant, where the C3c funnel insert is the
 *                             whole response (@c gap_reentry_dd_only).
 * @param carry_slipped_ambs   C3c funnel, inserted into under @p carry_invariant.
 * @param raw                  This epoch's raw-observable evidence (reads @c resumptions).
 * @param qc                   Knob snapshot (the @c gap_reentry_* fields).
 * @param local_carry          The leveling carry map, read-only: what a re-entry
 *                             threatens, and the source of the drain's "anything
 *                             to protect?" test (see above).
 * @param pl_now_s             This epoch's continuous-GPS seconds (the inject site's
 *                             @c pl_now_s, so the gap arithmetic is identical).
 * @param pl_max_gap_s         @c network.phase_leveling_max_gap_s.  Beyond it the
 *                             PASS-B inject skips the entry anyway, so there is
 *                             nothing for this response to prevent.
 * @param qc_cnt               Run-end counters, accumulated into.
 * @param relevel              Free-epoch latch, or nullptr when
 *                             @c cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend         Bridge-v2 pend map, or nullptr.  A fresh mark voids
 *                             any outstanding pend on the same (station, satellite),
 *                             as the raw-slip drains do.
 *
 * ## Ledger-driven population (@c SlipQcParams::ledger_reentry, card SID phase 3)
 *
 * Armed, the arc list is not this tier's own resumption list but the arc
 * ledger's admitted Gap-to-Reentered population (@p sid), and the raw verdict is
 * looked up per arc: the per-signal resumption for the exact (station,
 * satellite, signal) if one exists (the per-signal tier then owns the response),
 * otherwise the row resumption for the (station, satellite).  An arc that
 * neither tier saw has no verdict and is handled by
 * @c SlipQcParams::ledger_reentry_refloat, which routes it down the unmeasurable
 * path, so it is refloated only when @c gap_reentry_unmeasurable is also armed.
 * The raw loop still runs, and its detections, verdicts and traces are
 * unchanged, but it does not respond.  This drain owns the census of the whole
 * population (@c ledger_reentry_* in @ref SlipQcCounters), including the arcs it
 * hands to the per-signal tier, so the denominator is counted once, and four
 * dispositions partition it: freed, bridged, no_carry and pivot_skipped.
 *
 * Because the response key is the ledger arc's own key,
 * @c gap_reentry_unreachable is never incremented on this path and reads 0 when
 * armed.
 *
 * @param sid     This epoch's ledger-driven population, or nullptr / @c armed
 *                false for the historical raw-driven path (byte-identical).
 * @param sid_out Per-arc outcome sink for the unified "answered" census (R4), or
 *                nullptr when no roster was set up (every network run sets one up
 *                since card NOM step N3a).  Written in both states of
 *                network.ledger_reentry.
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
                                         RelevelPend>* relevel_pend = nullptr,
                                const ArcReentryEpoch* sid = nullptr,
                                ArcReentryOutcomeMap* sid_out = nullptr,
                                ArcLedger* arc_ledger = nullptr);

/**
 * @brief The gap re-entry response keyed on the signal instead of the row (card
 *        GRS, docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md).
 *
 * ## Population
 *
 * @ref networkSlipDrainGapReentry answers a @ref GapResumption, which is the raw
 * row being absent for at least one processed epoch.  The row is rebuilt every
 * epoch from whichever phase signals have a matching code, so a satellite that
 * keeps one signal through a dropout keeps its row: a receiver that loses lock on
 * the semi-codeless L1/L2 phases while the civil L5 tracks through re-enters with
 * an adjacent predecessor.  No resumption is written, no time-difference detector
 * covers the return epoch (MW needs an adjacent predecessor of the same pair,
 * IF-TD three consecutive epochs), and the carried ambiguities of the returning
 * phases would fold across the break.  With a third band present this case is
 * the rule rather than the exception, and @c raw_observed carries band 5 even on
 * a dual-frequency run, so the gap is not specific to the triple-frequency
 * posture.
 *
 * ## What differs from the row-level tier
 *
 *  - the level evidence is a geometry-free phase difference against a partner
 *    signal, snapshotted at the candidate's own last-seen epoch
 *    (@ref SigResumption), not the row's MW/GF pair step;
 *  - the LLI read is the returning signal's own, not the primary pair's, since
 *    the pair's flags could attribute the receiver's verdict to the wrong band;
 *  - an unmeasurable re-entry is freed by default here
 *    (@c SlipQcParams::gap_reentry_per_signal_unmeasurable), because a partial
 *    dropout with no witness is the damaging case: unlike after a whole-satellite
 *    gap, the carry of a satellite that never left is certain to be folded;
 *  - the response is band-scoped: only the returning signal is freed, because
 *    the other bands tracked through, and freeing them would add the
 *    response-inflicted slip cost that SPEC_lane_slip_response measures.
 *
 * Everything else is shared with the row-level tier through the same per-arc
 * responder: the `pl_bridged` protectable test on @p local_carry, the synthesized
 * @c StateKeys::makeAmbiguityKey, the pending-reseed mark, the @c free_ambs latch
 * with @c RelevelEpochState::kCauseGap, and the bridge-pend void.  Counters land
 * in @c SlipQcCounters::gap_sig, so the two tiers stay separately auditable and
 * each closes its own partition.
 *
 * Both edges are events (invariant #7): `T0_gap sig_resume` for every per-signal
 * re-entry, clean ones included, so the tier can be audited for what it declined
 * as well as for what it did, and `T0_gap reentry_relevel` with `scope=signal`
 * for each freed signal.
 *
 * ## The co-resumed share (@c SlipQcParams::gap_reentry_coresumed_share)
 *
 * The level test differences a candidate against a partner that tracked through,
 * so a step common to both is invisible to it.  A channel delay change at
 * re-acquisition is such a step: in the measured field case L1C and L5Q stepped
 * together by about 7.5 m, L1C read clean against L5Q and its carry was bridged
 * while the co-resumed L2W read shifted.  Armed (and only with the tier itself
 * active, @c SlipQcParams::coresumedShareActive), the resumptions of one
 * (station, satellite) at this epoch are one re-acquisition: the first of them
 * with a verdict of its own is the trigger, and every other one without a verdict
 * is freed through the same responder, bucketed under the trigger's primary cause
 * (so the tier's closure is unchanged), with the cause text `shared`, the event
 * `T0_gap reentry_relevel` with `scope=los_shared` naming the trigger, and the
 * sub-counts @c SlipQcCounters::gap_sig_share (@c CoresumedShareLedger).  A signal
 * that tracked through has no resumption and is never in a group; a group with no
 * verdict is bridged as without the knob; a signal an earlier drain of this epoch
 * already freed (it is in the free-epoch set) is left as clean and counted
 * `already_freed`.  Under the ledger the share is a verdict on a ledger arc: the
 * row-level drain's ledger loop routes such an arc here instead of counting it
 * bridged (@c SlipQcCounters::ledger_reentry_v_shared), with the same already-freed
 * exception.
 *
 * No-op unless both @c SlipQcParams::gap_reentry and
 * @c SlipQcParams::gap_reentry_per_signal are set.  Called in the slip-drain band
 * of PASS A immediately after the row-level drain, for the same reasons.
 *
 * Parameters are the row-level drain's, with the same contracts.
 *
 * With the ledger armed (@p sid, card SID phase 3), this tier responds only to
 * the ledger arcs whose exact (station, satellite, signal) has a per-signal
 * resumption this epoch; the row-level drain owns the census and every other
 * arc.  The `sig_resume` decoupling-edge event is emitted for every raw
 * per-signal resumption in both states.
 */
void networkSlipDrainGapReentryPerSignal(
    std::vector<NetStation>& stations, const std::vector<bool>& present,
    int i_lo, int n_sta_total, bool carry_invariant, bool carry_warm,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const RawSlipEpoch& raw, const SlipQcParams& qc,
    const std::map<std::tuple<int, std::string, std::string>, LocalCarry>& local_carry,
    double pl_now_s, double pl_max_gap_s, SlipQcCounters& qc_cnt,
    const SyncedEpoch& se, NetworkRunTraces& traces,
    const std::shared_ptr<spdlog::logger>& log,
    RelevelEpochState* relevel = nullptr,
    std::map<std::tuple<int, std::string, std::string>,
             RelevelPend>* relevel_pend = nullptr,
    const ArcReentryEpoch* sid = nullptr,
    ArcReentryOutcomeMap* sid_out = nullptr);

struct DeltaBridgeSolve {
    bool   ok{false};
    double dn1_cyc{0}, dn2_cyc{0};
    double shift1_m{0}, shift2_m{0};
    double var1_m2{0}, var2_m2{0};
};
/**
 * @brief Float 2×2 delta-bridge solve for one (MW, IF-TD) detector pair
 *        (SPEC_slip_relevel_walk §6).
 *
 * Given the confident zero-differenced wide-lane integer @f$\Delta N_{WL} =
 * \Delta N_1 - \Delta N_2@f$ (cycles) and the measured ionosphere-free phase
 * step @f$\delta_{IF} = \beta_1 \Delta N_1 - \beta_2 \Delta N_2@f$ (metres,
 * @f$\beta_j = \lambda_{WL} f_j/(f_1+f_2)@f$, with the identity
 * @f$\beta_1-\beta_2 = \lambda_{IF}@f$), solve the per-band deltas as floats.
 * The result is a bridge, not an integer fix: a wrong wide-lane integer can bias
 * the gauge but not lock it, and the propagated variance keeps the bridged prior
 * loose enough for the data to pull it back (invariant #1: the arc stays
 * catchable).  The band frequencies are derived from the two wavelengths alone
 * (@f$f_1 = \tfrac{c}{2}(1/\lambda_{WL} + 1/\lambda_{IF})@f$, exact per
 * satellite, GLONASS FDMA included), so the kernel needs no signal tables.
 *
 * @param dn_wl_cyc     Confident ZD wide-lane integer (cycles).
 * @param sigma_wl_cyc  Its 1σ confidence (cycles).
 * @param dif_m         Measured IF phase step, common-mode removed (m).
 * @param sigma_if_m    Its 1σ (m): the IF-TD driver's effective scale at the
 *                      sample's elevation, with a configured floor.
 * @param lambda_wl_m   Wide-lane wavelength (m).
 * @param lambda_if_m   (1,1) IF wavelength c/(f1+f2) (m).
 * @param max_abs_cyc   Reject when either |ΔN_j| exceeds this (cycles).
 * @param max_sigma_cyc Reject when either propagated σ(ΔN_j) exceeds this (cycles).
 * @return ok=false on any reject; else the per-band float deltas, the metre
 *         shifts λ_j·ΔN_j, and the per-band shift variances (m²).
 */
DeltaBridgeSolve networkSlipDeltaBridgeSolve(double dn_wl_cyc, double sigma_wl_cyc,
                                             double dif_m, double sigma_if_m,
                                             double lambda_wl_m, double lambda_if_m,
                                             double max_abs_cyc, double max_sigma_cyc);

/**
 * @brief Bridge v2 (pend-and-replace): execute one pended replace against the
 *        live carry map.  Depends only on its arguments (unit-test seam).
 *
 * Solves the 2×2 from the pend's (δIF, σIF) and its arrived MW integer, then
 * applies the corroboration gate of the §8.1 bridge truth audit
 * (SPEC_slip_relevel_walk): the bridged level (pre-slip + shift) must agree with
 * the current carry value, which is the interim v1 free re-level, within
 * @p corrob_cyc cycles per band.  In that audit correct bridges erred at most
 * 0.5 cycles against truth and wrong ones 6–113 cycles, while the free level sits
 * within about 2 cycles, so agreement separates the two populations.  On Applied
 * the carry values are replaced (value = pre + shift, var = pre_var + bridge
 * var); coupling and last_seen are left untouched (the local-step mutation
 * rule).  Every other outcome leaves the carry as found, and the arc keeps its
 * free re-level.
 */
enum class BridgeReplaceOutcome { Applied, KernelReject, NoCarry, CorrobReject };
BridgeReplaceOutcome networkSlipBridgeReplace(
    const RelevelPend& pd, int sta, const std::string& sat,
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
    DeltaBridgeSolve* solved = nullptr);

/**
 * @brief Bridge v3: staged closed-form cascade over the pend's equation menu
 *        (SPEC_slip_relevel_walk §8.2 follow-on: the better second equation).
 *
 * Stage 1 resolves the IF-pair plane (ΔN1, ΔN2):
 *  - E2 route (@c has_dn): the v2 2×2 from the MW wide-lane integer, unchanged
 *    (the integer, when it arrives, is the stronger equation).  When the GF row
 *    is also present, the E4 measurement over-determines the plane, and the
 *    standardized plane residual @f$|d_{GF} - (\lambda_1\Delta N_1 -
 *    \lambda_2\Delta N_2)| / \sigma@f$ screens a wrong detector-rounded integer
 *    (±1 WL sits 2.5–5σ off the float plane): the bridge is rejected above
 *    @c kBridgePlaneResidMax.
 *  - E1+E4 route (@c has_gf, no integer yet): solve
 *    @f$[\beta_1, -\beta_2; \lambda_1, -\lambda_2][\Delta N_1;\Delta N_2] =
 *    [\delta_{IF}; d_{GF}]@f$, same-epoch and float-only, with no integer
 *    hypothesis at risk.  det ≈ −0.046 m² (GPS) / −0.048 m² (GAL); at cm-class
 *    inputs σ(ΔN) ≈ 0.15–0.3 cycles.  The two measurements share the same band
 *    TDs, so the propagation carries an off-diagonal
 *    @f$\rho\,\sigma_{IF}\sigma_{GF}@f$.  In the phase-noise limit ρ ≈ +0.97,
 *    and the solve coefficients have opposite signs, so the shared noise
 *    partially cancels: in the pure-phase limit, independent propagation
 *    (ρ = 0) overstates σ(ΔN1) about 3.7× (pinned by a unit test) and σ(ΔN2)
 *    about 4.7×.  That costs yield through the σ cap, not safety.  ρ must not
 *    exceed the physical limit: an over-set ρ understates σ, which is the
 *    dangerous direction.  The shipped 0.6 (@ref kBridgeGfIfRho) is a
 *    conservative middle.
 *
 * The band-3 chain from the EWL-MW integer (stage 2, bridge v3 E3) is retired
 * with @c network.cycle_slip.relevel_bridge_ewl.  The kernel
 * works in floats throughout and never rounds: rounding its output would bring
 * back the wrong-integer risk of the mw_only variant behind a small propagated σ.
 */
struct BridgeV3Solve {
    bool   ok{false};
    bool   used_gf{false};         ///< Stage 1 took the E1+E4 route (no MW integer).
    double dn1_cyc{0}, dn2_cyc{0};
    double shift1_m{0}, shift2_m{0};
    double var1_m2{0}, var2_m2{0};
    double plane_resid_sigma{-1.0};///< E2-vs-E4 overdetermination residual (σ); <0 = not formed.
};
/// Conservative E1/E4 error correlation (shared band-TD content; phase limit 0.97).
inline constexpr double kBridgeGfIfRho = 0.6;
/// Plane-residual reject threshold (σ) for a wrong detector-rounded WL integer.
inline constexpr double kBridgePlaneResidMax = 4.0;
BridgeV3Solve networkSlipBridgeV3Solve(const RelevelPend& pd, double rho_if_gf,
                                       double max_abs_cyc, double max_sigma_cyc);

/**
 * @brief Bridge v3 replace: the v2 outcome ladder over the IF pair's two bands.
 *
 * Same discipline as @ref networkSlipBridgeReplace: kernel caps, then per-band
 * free-level corroboration, then a value-and-var-only mutation.  There is no
 * partial apply: a corroboration failure on either band rejects the whole
 * bridge, because a half-replaced LOS would mix pre-slip and free re-level
 * frames.
 */
BridgeReplaceOutcome networkSlipBridgeV3Replace(
    const RelevelPend& pd, int sta, const std::string& sat,
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    double rho_if_gf, double max_abs_cyc, double max_sigma_cyc, double corrob_cyc,
    BridgeV3Solve* solved = nullptr);

/**
 * @brief IF-phase triple-difference slip driver: pool the PASS-A deferred
 *        ΔΔφ_IF caches across stations by signal pair, remove the network
 *        common-mode median, threshold elevation-aware against an
 *        empirical-Bayes blend of the pooled robust scale and each
 *        (station, pair)'s own EWMA scale, then refloat flagged arcs and fold
 *        clean survivors back into the EWMA (slips excluded).
 *
 * Gated inside on network.cycle_slip.if_phase_td; call it unconditionally.  The
 * five threshold knobs are read inside, and so is
 * network.cycle_slip.if_phase_td_reseed_on_flag (card ARL): armed, a flag keeps
 * the satellite's flag-epoch gradient (IfPhaseSlipDetector::reseedSatellite)
 * instead of restarting its chain, so the next epoch is evaluated too.  The algorithm description, the
 * refloat_pair lambda and the PairSample struct are in the driver body in the
 * .cpp.  The per-(station, pair) EWMA scale is cross-epoch state kept in
 * NetStation::if_pair_scale and updated through @p stations.  The driver also
 * folds in the detectors' reference-satellite common-mode reset keys
 * (pa.sta_if_resets) and feeds @p carry_slipped_ambs as the MW drain does (C3c).
 *
 * @param pa                 PASS-A buffers (reads sta_if_resets).
 * @param config_            Config (if_phase_td gate + 5 knobs read inside).
 * @param stations           Station roster: detector arc resets, EWMA scale
 *                           updates, StateManager pending-reseed marks.
 * @param present            Per-station PASS-A presence.
 * @param i_lo               First non-pivot station index (0 under prioritized datum).
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry armed (C3c feed gate).
 * @param carry_warm         Legacy carry warm gate (refloat vs diagnostic).
 * @param carry_slipped_ambs The per-epoch C3c slip-event set, by non-const
 *                           reference (see networkSlipDrainMwRefloat).
 * @param trace_flag_rates   B8 flag-rate counters armed.
 * @param fr_if_slip         T1 flag-rate counter: the ctx.counters.fr_if_slip
 *                           instance, by reference (feeds the teardown summary).
 * @param se                 Current synced epoch (event/trace rows, log lines).
 * @param traces             Trace unit (T1_if_slip event rows and the deep
 *                           if_slip_trace CSV stream).
 * @param log                Run logger.
 * @param refsat_mark_sats   Edge-1 piece-2 queue (@c network.mark_refsat_reset_disc;
 *                           default off, and then nullptr, which keeps the run
 *                           byte-identical).  When non-null and the carry is warm,
 *                           every satellite whose reference-path arc is reset here
 *                           is inserted for a per-satellite discontinuity mark at
 *                           its next actual emit (@c NetworkSolPrep drains it
 *                           through the same mark-and-serialize path as the B1
 *                           retro-bump, so it lands as an @c R record).
 *
 *                           An explicit mark is used instead of the emitter's
 *                           replay-safe bias-jump test, on which eq34_reattach and
 *                           anchor handovers rely without marking (see their
 *                           handover block in NetworkSolPrep.cpp).  Most
 *                           reference-path slips step the emitted bias past the
 *                           jump gate and are caught by that test (the reset
 *                           population of HANDOFF_edge_response_queue.md §1.1).
 *                           The excursion onsets of
 *                           SPEC_level_information_budget §25.3, however, are
 *                           enriched in refsat_reset events and carry no
 *                           phase-bias value step: that subset re-references
 *                           without a value step, which the jump test cannot
 *                           see, and this mark targets it.  The mark cannot be
 *                           reconstructed from the serialized epoch (replay does
 *                           not re-run slip QC), so it travels as an R record
 *                           instead of being recomputed.
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
                           /// Common-band scope verdicts for this epoch (§14).  Null
                           /// (the knob off): this scope refuses no band, and the pair
                           /// refloat frees both bands.
                           const CommonBandEpoch* common_band = nullptr,
                           /// Pair-width tally sink (§14.10): run-end counts per pair
                           /// kind (evaluated / flagged / refloated).  Filled whatever
                           /// the knob state (invariant #4); a `first` run reports the
                           /// one watched pair.  Null skips it.
                           IfTdPairTallies* pair_tally = nullptr);

/**
 * @brief Injected @c free_epoch arc response (event harness, Study 1).
 *
 * The T4 attribution erase and a detector refloat are two different responses
 * to an arc level that is not trusted, and the harness measured their effects
 * 11× apart on the same clean arc (docs/SPEC_event_harness.md §9).  This drain
 * makes the detector response injectable: an @c arc_erase event carrying
 * @c "mode": "free_epoch" is routed here instead of to the T4 block, and does
 * what @c refloat_signal does for a detected slip: the C3c funnel insert under
 * @p carry_invariant; the pending-reseed mark, the free-epoch latch and the cause
 * tag (@c kCauseT4Attrib) under @p carry_warm; and the bridge-pend void that a
 * fresh mark implies.  Unlike the T4 erase, it does not erase the @c LocalCarry
 * entry, co-erase the LOS iono carry or queue the RAIM funnel; that difference
 * is what the comparison measures.
 *
 * Called after the other PASS-A drains, so the free-epoch latch it fills is read
 * by the PASS-B leveling inject of the same epoch.  The T4 block cannot host it:
 * post-fit RAIM runs after that inject, so a free_ambs insert there would be
 * cleared before any site read it.
 *
 * Harness-only: it acts only on a @c free_epoch directive that @c network.inject
 * staged this epoch, and @p arc_free is empty on every run without the injector,
 * so those runs are byte-identical by construction (invariant #6).
 *
 * @param arc_free           This epoch's staged directives (every mode; the
 *                           non-@c free_epoch ones are skipped here and taken
 *                           by the T4 block).
 * @param stations           Station roster; @c sm is modified (pending-reseed marks).
 * @param present            Per-station PASS-A presence.
 * @param n_sta_total        Station count.
 * @param carry_invariant    Card-4 DD invariant carry vehicle armed.
 * @param carry_warm         The legacy phase-leveling carry is warm.
 * @param carry_slipped_ambs C3c funnel, inserted into under @p carry_invariant.
 * @param inject             Injector ledger: this drain closes @c n_applied,
 *                           @c fired_by_kind and @c arc_erase_mode_acted for the
 *                           directives it consumes (the T4 block closes the
 *                           others).  A directive that matches no live ambiguity
 *                           stays open and reaches the run-end never-fired hard
 *                           error.
 * @param se                 This epoch (event rows).
 * @param traces             Trace bundle; one INJECT row per acted arc.
 * @param log                Logger.
 * @param relevel            Free-epoch latch, or nullptr when
 *                           @c cycle_slip.relevel_free_epoch is off (then the
 *                           mark alone cannot deliver the free epoch, and the
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

struct NetworkRun;

/**
 * @brief Fills the slip-QC channel's parameters from the configuration and sizes its per-station history.
 *
 * The channel (run.engines.slip_qc) carries the knobs, the per-station cross-epoch state, the
 * epoch's raw evidence and the run-end counters into the PASS-A slip seam and the assembly
 * pre-filter; it is sized once so the per-station EWMAs survive the whole run.  Setup builder
 * (card NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.engines.slip_qc.params and run.engines.slip_qc.hist.
 */
void networkSlipQcConfigure(NetworkRun& run);

class LogManager;

/**
 * @brief Builds the raw-slip path's wavelength provider when any of its consumers is armed.
 *
 * Called after NetworkRunTraces::openAll: the raw-slip trace it opens is one of the consumers.
 * Setup builder (card NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.engines.slip_qc.utils.
 * @param log_manager_ The application's logging manager (Application::log_manager_).
 */
void networkSlipQcArmWavelengthProvider(NetworkRun& run, LogManager& log_manager_);

} // namespace GnssProcessor

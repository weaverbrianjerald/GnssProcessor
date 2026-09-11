#pragma once
/**
 * @file NetworkRawObservables.hpp
 * @brief Shared per-epoch RAW cycle-slip observables (Melbourne-Wübbena,
 *        geometry-free, phase-minus-code, RINEX LLI) for the network run loop,
 *        plus the empirical qualification pass that Cards L (LLI
 *        corroboration) and G (gap-coincident instrumentation) decide on.
 *
 * ## Why this seam exists
 *
 * Before the 2026-07-21 slip-QC hardening the raw MW/GF math lived INSIDE the
 * `network.trace_slip_raw` writer (`ApplicationNetworkAssembly.cpp`), so it was
 * (a) unavailable to any decision path and (b) only computed when the trace was
 * armed.  Two hardening cards need the same numbers as *evidence*, not as a
 * trace: the LLI-corroboration gate (Card L) and the arc-resumption instrument
 * (Card G).  Duplicating the math in the drains would have been three
 * independent definitions of "the raw step"; instead it is computed ONCE per
 * epoch here, when ANY consumer needs it, and the trace writer became a pure
 * consumer (its rows are byte-unchanged when it is the only consumer).
 *
 * ## Threading contract
 *
 * Everything here is SERIAL and station-ordered (deterministic), called from
 * the slip-QC seam after the PASS-A `present[]` fold and BEFORE the drains and
 * the observability pre-filter — so a satellite about to be pruned is still
 * measured, exactly as the original trace writer promised.
 *
 * ## Empirical-threshold discipline (project rule 4)
 *
 * Every gate in this file derives from the observable stream itself — a robust
 * per-station scale of that station's OWN epoch-to-epoch steps (1.4826·median
 * of the clean survivors, folded into an EWMA), floored by a PHYSICAL integer
 * bound kept in raw metres (half a wide-lane cycle for MW, a few cm for GF).
 * No stochastic-model σ enters anywhere.  This mirrors the IF-TD pool arm in
 * NetworkSlipQc.cpp, deliberately: one empirical pattern, two instruments.
 */

#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief RAW slip observables for one (station, satellite) at one epoch.
 *
 * Absolute quantities (NOT differences) — exactly what the `network_slip_raw`
 * trace has always written.  The epoch-to-epoch STEP is formed against the
 * previous epoch's row for the same (station, satellite) by
 * @ref networkQualifyRawObservables.
 *
 * The primary pair is the two lowest DISTINCT bands whose phase AND matching
 * code ("C" + signal tail) are both present.  When no distinct second band
 * exists, @ref dual is false and only @ref l1_c1_m is meaningful.
 */
struct RawSlipObs {
    std::string satellite;        ///< Satellite id ("G10").
    std::string sig1;             ///< Primary (lowest-band) phase signal ("L1C").
    std::string sig2;             ///< Second distinct band ("L2W"); empty when !dual.
    double elev_deg{0.0};         ///< Elevation (deg) at this epoch.
    bool   dual{false};           ///< A valid distinct second band with a finite wavelength.
    double mw_m{0.0};             ///< Melbourne-Wübbena combination (m); 0 when !dual.
    double gf_m{0.0};             ///< Geometry-free L1−L2 phase (m); 0 when !dual.
    double gf_code_m{0.0};        ///< Geometry-free P1−P2 code (m); 0 when !dual.  (CS-GF authority input; not traced.)
    double gf_denom{0.0};         ///< γ−1 = (λ2/λ1)²−1 > 0 (dispersive scale of the pair); 0 when !dual.
    double lam1_m{0.0};           ///< Primary-band wavelength of the pair (m) — the LV-6 census's exact per-pair TECU conversion (the (L2,L5) fallback pair is a 65 % mis-scale under an assumed-L1 constant); 0 when !dual. Not traced.
    double l1_c1_m{0.0};          ///< sig1 phase-minus-code (m); always meaningful.
    double lambda_wl_m{0.0};      ///< Wide-lane wavelength 1/(1/λ1 − 1/λ2) (m); 0 when !dual.
    int    lli1{0};               ///< RINEX LLI byte for sig1 (bit 0 = loss of lock).
    int    lli2{0};               ///< RINEX LLI byte for sig2 (0 when !dual).
};

/**
 * @brief CS-GF: one LOS's carrier-smoothed GF-code slant-iono authority
 *        (QUEUE 8b successor — docs/SPEC_iono_carry_8b.md §6.2).
 *
 * A Hatch-class recursive estimate of the slant iono (metres at the primary
 * band, in the code channel's realization ι̃ = ι + d_r,GF − d^s_GF): the GF
 * PHASE delta propagates the state between epochs (it tracks the true iono
 * change exactly, ambiguity-constant), and each epoch's GF CODE sample pulls
 * it toward the code channel at Kalman weight.  Level-independent BY
 * CONSTRUCTION — the leveling carry never touches it, which is the entire
 * point: the solve-fed iono carry was measured accumulating the level-locked
 * displacement (i1, §6.1), while this accumulates the code evidence the
 * displacement never drains without.  Reset on the qualify pass's own
 * empirical step gates (slip) and on lost adjacency (gap) — self-healing,
 * NOT snapshotted (the RawSlipHistory exemption rationale).
 */
struct IonoCsState {
    double val{0.0};   ///< Smoothed slant iono (m, primary band, code realization).
    double var{0.0};   ///< Its variance (m²; formal — code multipath correlation makes it optimistic, the inject floor covers).
    int    n{0};       ///< Samples since the last reset (0 = never initialized).
    double lam1_m{0.0};///< The pair's primary-band wavelength (m), stamped at the advance — the LV-6 census converts val with THIS pair's frequency, never an assumed L1.
};

/// Per-carrier-step variance added by the GF-phase propagation
/// ((2σ_φ/denom)² class; conservative for the low-elevation tail).
inline constexpr double kIonoCsPhaseStepVar = 1e-4;   // m²
/// Zenith 1σ of one GF-code slant-iono sample: √2·σ_P0/denom at σ_P0 = 0.3 m,
/// GPS L1/L2 denom 0.6469 (the seed-noise analysis' 0.66 m).  Elevation-mapped
/// 1/sin at the call site.  A variance model for the estimator (stochastic-
/// model class), not a detector threshold — the MW-thresholds-empirical
/// directive does not apply to it.
inline constexpr double kIonoCsCodeSigmaZenith = 0.66; // m

/**
 * @brief One CS-GF recursion step (pure, unit-testable).
 *
 * @param cs            The LOS's state (reset or advanced in place).
 * @param i_code_m      This epoch's GF-code slant-iono sample −gf_code/denom (m).
 * @param di_carrier_m  The adjacent-epoch GF-phase delta d_gf/denom (m);
 *                      meaningful only when @p adjacent.
 * @param adjacent      An immediately-adjacent predecessor existed (verdict.measured).
 * @param stepped       The qualify pass's empirical MW/GF gate tripped (slip class).
 * @param sigma_i_code_m 1σ of @p i_code_m (elevation-mapped).
 * @return true when the step RESET the state (gap / slip / first sighting).
 */
bool networkIonoCsUpdate(IonoCsState& cs, double i_code_m, double di_carrier_m,
                         bool adjacent, bool stepped, double sigma_i_code_m);

/**
 * @brief Per-(station, satellite) adjudication of THIS epoch's raw step.
 *
 * Produced by @ref networkQualifyRawObservables against the previous
 * IMMEDIATELY-ADJACENT processed epoch.  @ref measured is false when there is
 * no adjacent predecessor (first sighting, or a gap — the gap case is reported
 * separately as a @ref GapResumption), in which case the step is structurally
 * unmeasurable and @ref stepped is false: absence of evidence is never
 * evidence of a slip, and never evidence of its absence either.
 */
struct RawSlipVerdict {
    bool   measured{false};   ///< An adjacent-epoch predecessor row existed (a step could be formed).
    bool   stepped{false};    ///< @ref mw_step || @ref gf_step.
    bool   mw_step{false};    ///< |ΔMW| exceeded the empirical MW gate.
    bool   gf_step{false};    ///< |ΔGF| exceeded the empirical GF gate.
    double d_mw{0.0};         ///< ΔMW across the epoch (m); 0 when !measured or !dual.
    double d_gf{0.0};         ///< ΔGF across the epoch (m); 0 when !measured or !dual.
    double gate_mw{0.0};      ///< Effective MW gate applied (m) — max(k·scale, floor).
    double gate_gf{0.0};      ///< Effective GF gate applied (m).
    /// The adjacent predecessor used a DIFFERENT (sig1, sig2) pair, so @ref d_mw
    /// differences two MW combinations built on different wide-lane wavelengths.
    /// @ref measured stays true (the step exists and the slip cards want it), but
    /// any consumer that converts @ref d_mw into CYCLES of a specific lane must
    /// refuse the row — the lane it would be compared against is not the lane the
    /// step was formed on.
    bool   pair_changed{false};
};

/**
 * @brief Card G: one arc that re-appeared after at least one missed epoch.
 *
 * "Missed" is measured in PROCESSED epochs (the run's own loop index), so a
 * decimated or start-offset run reports gaps in the units its estimator
 * actually sees.  @ref shifted is the poison signature: the raw level moved
 * across the break by more than the empirical gate, i.e. any state that
 * BRIDGED the break is now carrying a stale value.
 */
struct GapResumption {
    int         station{-1};      ///< App station index.
    std::string satellite;        ///< Satellite id.
    std::string sig1, sig2;       ///< Primary pair at re-entry (sig2 empty when !dual).
    long long   gap_epochs{0};    ///< Processed epochs missed (≥ 1).
    bool        measurable{false};///< A pre-gap raw row with a dual pair existed (a level shift COULD be seen).
    bool        shifted{false};   ///< |ΔMW| or |ΔGF| across the break exceeded the empirical gate.
    double      d_mw{0.0};        ///< MW level change across the break (m).
    double      d_gf{0.0};        ///< GF level change across the break (m).
    /// RINEX LLI byte for @ref sig1 ON THE RETURN EPOCH (bit 0 = loss of lock).
    ///
    /// The receiver's own verdict about the break, and the ONLY evidence a
    /// resumption has when @ref measurable is false (no pre-gap dual-band level
    /// to difference).  Carried here rather than re-derived at the response
    /// site: the raw row it comes from is already the one this resumption was
    /// built on, and re-looking it up in `modeled` would be a second definition
    /// of "the LLI of this arc at re-entry".  Measured on the DOY-129 field
    /// chain, the BSHM00ISR/G22 tow-271200 poison case carries LLI set on BOTH
    /// bands while no MW/IF detector fires that epoch.
    int         lli1{0};
    int         lli2{0};          ///< …for @ref sig2 (0 when the return row is single-band).
};

/**
 * @brief Cross-epoch per-station state for the raw-observable instruments.
 *
 * One entry per app station, sized once at run start and mutated ONLY by the
 * serial qualification pass.  Deliberately NOT part of the RTB-7(a) state
 * snapshot: this is slip-QC scratch state that self-heals within a handful of
 * epochs (the EWMAs re-converge, `prev` refills on the next epoch), and a warm
 * resume that rebuilt it exactly would still see a one-epoch unmeasurable gap
 * at the resume boundary.  Recording the exemption here rather than in
 * NetworkCarryState keeps the snapshot's three-place rule intact.
 */
struct RawSlipHistory {
    /// The previous row for a satellite plus the processed-epoch index it came from.
    struct Prev {
        RawSlipObs obs;
        long long  epoch_idx{-1};
    };
    std::map<std::string, Prev> prev;   ///< Previous raw row per satellite.

    // --- Card L: two-state LLI credibility (never "ignore" — see M1/M2) ---
    double    lli_frac_ewma{0.0};   ///< EWMA of the fraction of this station's tracked satellites asserting LLI.
    long      lli_frac_count{0};    ///< Epochs folded into the EWMA (hysteresis arming gate).
    bool      lli_credible{true};   ///< Two-state verdict: true ⇒ an uncorroborated LLI still acts.
    long long lli_demotions{0};     ///< credible → corroboration-required transitions over the run.
    long long lli_promotions{0};    ///< corroboration-required → credible transitions over the run.

    // --- Card L/G: robust EWMA scales of this station's OWN raw steps (m) ---
    double mw_step_scale{0.0};      ///< EWMA of 1.4826·median(|ΔMW|) over clean satellites.
    long   mw_step_count{0};        ///< Clean samples folded (0 ⇒ use this epoch's own robust median).
    double gf_step_scale{0.0};      ///< EWMA of 1.4826·median(|ΔGF|) over clean satellites.
    long   gf_step_count{0};        ///< Clean samples folded.

    // --- Card L/M4: per-station LLI accounting for the top-offenders line ---
    long long lli_asserted{0};      ///< (satellite, epoch) LLI assertions seen.
    long long lli_acted{0};         ///< Assertions that drove a refloat / C3c funnel insert.
    long long lli_suppressed{0};    ///< Assertions suppressed as uncorroborated at a non-credible station.
    long long lli_corr_mw{0};       ///< Assertions corroborated by an MW step.
    long long lli_corr_gf{0};       ///< Assertions corroborated by a GF step (and not MW).
    long long lli_unmeasurable{0};  ///< Assertions with no measurable step (first sighting / gap / single-band).
    long long lli_offpair{0};       ///< Assertions whose LLI'd band sits OUTSIDE the primary pair (unwitnessable by MW/GF).
};

/**
 * @brief Everything the drains and the run TU read about THIS epoch's raw
 *        observables.  Freshly cleared every epoch — never hoisted for reuse.
 */
struct RawSlipEpoch {
    /// Per station, in `modeled[i]` order, one row per satellite that yielded a
    /// usable primary signal.  The trace writer walks exactly this, so its rows
    /// stay byte-identical to the pre-seam inline computation.
    std::vector<std::vector<RawSlipObs>> rows;
    /// Per station, satellite → this epoch's step adjudication.
    std::vector<std::map<std::string, RawSlipVerdict>> verdict;
    /// Per station, Card L credibility verdict used for THIS epoch's decisions.
    /// char (not bool) purely for vector<bool> hygiene; 1 = credible.
    std::vector<char> lli_credible;
    /// Per station, this epoch's raw LLI-set fraction (telemetry / log only).
    std::vector<double> lli_frac;
    /// Card G: arcs that re-appeared after ≥ 1 missed processed epoch.
    std::vector<GapResumption> resumptions;
    /// (station index, satellite) pairs whose raw level stepped this epoch —
    /// the cheap membership test the D5 attribution site (Card G1c) needs.
    std::set<std::pair<int, std::string>> stepped;
    /// True once @ref networkComputeRawObservables has run this epoch (the
    /// consumers' "is there evidence at all" guard).
    bool computed{false};

    /// Size the per-station vectors and clear the per-epoch content.
    void reset(std::size_t n_sta) {
        rows.assign(n_sta, {});
        verdict.assign(n_sta, {});
        lli_credible.assign(n_sta, 1);
        lli_frac.assign(n_sta, 0.0);
        resumptions.clear();
        stepped.clear();
        computed = false;
    }
};

/**
 * @brief Snapshot of the Card L / Card G knobs, taken once per run.
 *
 * All defaults are the OFF/inert values: with @ref lli_corroborate and
 * @ref gap_instrument both false the qualification pass still runs (cheaply)
 * only when a consumer asked for it, and changes NOTHING — the drains ignore
 * the verdicts and the counters stay zero.
 */
struct SlipQcParams {
    // --- Card L (network.cycle_slip.mw_lli_*) ---
    bool   lli_corroborate{false};  ///< Master arm for the LLI qualification.
    double chatter_frac_hi{0.6};    ///< LLI-set fraction EWMA above which a station is demoted.
    double chatter_frac_lo{0.3};    ///< …and below which it is promoted back (hysteresis).
    int    chatter_min_epochs{1};   ///< Epochs of EWMA history before the verdict may change.
    double chatter_alpha{0.4};      ///< EWMA weight of the current epoch's fraction.
    double k_mw{5.0};               ///< MW step gate = max(k_mw·scale, floor_cyc·λ_WL).
    double k_gf{5.0};               ///< GF step gate = max(k_gf·scale, floor_m).
    double mw_floor_cyc{0.5};       ///< Physical MW floor in WIDE-LANE cycles (audit's support test).
    double gf_floor_m{0.04};        ///< Physical GF floor in metres (audit's support test).
    double step_alpha{0.15};        ///< EWMA weight for the per-station step scales.

    // --- Card G (network.cycle_slip.gap_instrument) ---
    bool   gap_instrument{false};   ///< Master arm for the arc-resumption instrument.

    // --- Card G RESPONSE (network.cycle_slip.gap_reentry_relevel) ------------
    /// Master arm for the gap-RE-ENTRY slip response (docs/SPEC_slip_qc_hardening
    /// §G3/W1 — the follow-up card Card G's own decision rule named once
    /// `gap_shifted_bridged` came back nonzero: 88 arcs on `runs/fldf3_pl2`).
    ///
    /// The instrument and the response consume the SAME @ref GapResumption
    /// records, so this arms the resumption collection independently of
    /// @ref gap_instrument — an operator who wants the fix must not have to arm
    /// a diagnostic to get it, and the response must not silently become inert
    /// if the diagnostic is turned off.  The drain itself lives in
    /// `networkSlipDrainGapReentry` (slip/NetworkSlipQc.cpp).
    bool   gap_reentry{false};
    /// Sub-gate: a resumption whose RETURN epoch asserts RINEX LLI on either
    /// band of the primary pair counts as SLIPPED even when the raw MW/GF level
    /// did not clear the empirical gate.  DEFAULT ON — it is the receiver's own
    /// loss-of-lock verdict, it is the only evidence an UNMEASURABLE resumption
    /// can carry, and it is exactly what the field's poison case asserts.
    bool   gap_reentry_lli{true};
    /// Sub-gate: treat an UNMEASURABLE resumption (no pre-gap dual-band level,
    /// so no level shift could be seen and no LLI was asserted) as slipped.
    /// DEFAULT OFF — absence of evidence is not evidence of a slip, and the
    /// unmeasurable-bridged population is the smaller one (24 arcs vs 88 on the
    /// field chain).  Arming it is the conservative-reset posture.
    bool   gap_reentry_unmeasurable{false};

    // --- Bridge v3 E4 (network.cycle_slip.relevel_bridge_gf) ---
    /// The GF-row capture consumes the qualified verdicts (d_gf + the empirical
    /// gf_step_scale), so it arms BOTH the compute and the qualify pass.
    bool   bridge_gf{false};

    // --- CS-GF authority (network.iono_carry_source = smoothed_gf) ---
    /// The carrier-smoothed GF-code slant-iono authority consumes the verdicts
    /// (d_gf carrier deltas + the empirical step gates as its slip reset), so
    /// it arms both passes too.
    bool   iono_cs{false};

    // --- Corroborated retap (network.cycle_slip.relevel_retap_corroborate) ---
    /// The SB_RAIM corroborated keep consumes the qualified verdicts (measured
    /// AND not stepped), so it arms both passes.  Without this, an armed
    /// corroborate would find `computed == false` every epoch and refuse every
    /// keep — a silent degeneration to refloat-always that only the refusal
    /// counters would reveal (SPEC_lane_slip_response §3).
    bool   retap_corr{false};

    /// SPEC_lane_slip_response §7.3 — the carry-domain lane observatory
    /// (@c network.cycle_slip.relevel_lane_snap).  LOAD-BEARING in BOTH
    /// predicates below, for two different reasons:
    ///   - @ref anyConsumer decides whether the raw observables are computed at
    ///     all (a null @c slip_qc.utils leaves every row absent);
    ///   - @ref cardsArmed decides whether the QUALIFICATION pass runs, and the
    ///     observatory reads @c RawSlipVerdict::measured / @c d_mw, which only
    ///     that pass ever writes.
    /// Omitting it from cardsArmed was measured on the first smoke run: rows
    /// existed (0 "no MW pair") while every verdict stayed default-constructed,
    /// so the card reported 159 attempts and 0 measured — ARMED and inert, the
    /// exact shape standing invariant #4 exists to expose.  The closure identity
    /// still held, which is why the skip channel had to be separable.
    bool   lane_snap{false};

    /// True when any consumer needs the raw observables computed this run.
    bool anyConsumer(bool trace_slip_raw) const {
        return trace_slip_raw || lli_corroborate || gap_instrument || bridge_gf
            || iono_cs || retap_corr || lane_snap || gap_reentry;
    }
};

/**
 * @brief Run-end counters for the slip-QC hardening cards (standing invariant
 *        #4 — inert wiring is only visible through counters).
 *
 * Card E's counters are DETECTIONS, deliberately separate from refloats: a
 * cold run detects and does not refloat, and reporting only refloats made the
 * MW/LLI arm look dead (the L3 defect this card fixes).
 */
struct SlipQcCounters {
    // --- Card E: MW-detect / LLI detection vs action ---
    long long mw_detections{0};     ///< (station, satellite) wide-lane slip detections.
    long long mw_refloats{0};       ///< …of those, warm epochs that actually refloated.
    long long lli_detections{0};    ///< (station, satellite) LLI detections that ACTED (post-Card-L).
    long long lli_refloats{0};      ///< …of those, warm epochs that actually refloated.

    // --- Datum-station slip detect (network.cycle_slip.datum_station_detect) ---
    // Deliberately SEPARATE from the Card-E populations above: those count
    // non-pivot detections whose action is a local refloat; these count the
    // gauge-pinned datum station, whose action is a network re-reference.
    long long datum_slip_detections{0}; ///< (datum station, satellite) MW/LLI slip detections (queue edge).
    long long datum_slip_rerefs{0};     ///< …network re-references APPLIED (pb reseed + all-station amb reseed + disc mark).

    // --- Card L: LLI qualification ---
    long long lli_asserted{0};      ///< Raw (station, satellite) LLI assertions seen.
    long long lli_suppressed{0};    ///< Suppressed: uncorroborated at a non-credible station.
    long long lli_corr_mw{0};       ///< Corroborated by an MW step.
    long long lli_corr_gf{0};       ///< Corroborated by a GF step (MW quiet or unavailable).
    long long lli_unmeasurable{0};  ///< No measurable step (first sighting / gap / single band).
    /// Assertions whose LLI'd band lies OUTSIDE the primary dual-frequency pair
    /// the shared observables are formed on — MW/GF structurally cannot witness
    /// them.  A nonzero count is the trigger to widen the corroboration
    /// primitive to every tracked pair rather than a reason to distrust it.
    long long lli_offpair{0};
    long long lli_credible_epochs{0};   ///< (station, epoch) pairs judged credible.
    long long lli_demoted_epochs{0};    ///< (station, epoch) pairs judged corroboration-required.

    // --- Card G: arc resumptions across a tracking break ---
    //
    // UNITS (spec-followups N3/H2 — the two levels are NOT interchangeable and
    // the summary line must never let a reader mix them):
    //   * the first three count RESUMPTIONS — one per (station, satellite)
    //     re-entry, the unit the raw MW/GF level shift is measured in;
    //   * the six bridge-classification counters count ARCS — one per
    //     (station, satellite, SIGNAL), because carry chords are per-arc, so a
    //     dual-band satellite returning once contributes two independently
    //     bridgeable arcs.
    // Hence `gap_shifted` (resumptions) is smaller than
    // `gap_shifted_bridged + gap_shifted_fresh` (arcs) by construction — the
    // 2026-07-21 run's "11 shifted … 22 fresh" was exactly that factor of two,
    // not a lost count.  The arc ledger mirrors the resumption three-way split
    // exactly — SHIFTED / measured-and-unshifted (clean) / UNMEASURABLE, each
    // closed as bridged+fresh — so every arc is counted once and an unmeasurable
    // resumption is never reported as clean.
    long long gap_resumptions{0};       ///< RESUMPTIONS: arcs re-established after ≥ 1 missed processed epoch.
    long long gap_unmeasurable{0};      ///< RESUMPTIONS: …with no pre-gap dual-band level to compare against.
    long long gap_shifted{0};           ///< RESUMPTIONS: …whose raw MW/GF level shifted beyond the empirical gate.
    long long gap_shifted_bridged{0};   ///< ARCS of a SHIFTED resumption that re-entered with BRIDGED carry state (the poison case).
    long long gap_shifted_fresh{0};     ///< ARCS of a SHIFTED resumption that re-entered as a fresh float (benign).
    long long gap_clean_bridged{0};     ///< ARCS of a MEASURED-and-unshifted resumption that re-entered bridged (the intended behaviour).
    long long gap_clean_fresh{0};       ///< ARCS of a MEASURED-and-unshifted resumption that re-entered fresh (a conservative reset).
    /// ARCS of an UNMEASURABLE resumption (no pre-gap dual-band level) that
    /// re-entered bridged.  Deliberately NOT folded into @ref gap_clean_bridged:
    /// state that bridged a break nobody could check is not "clean", it is
    /// UNKNOWN, and G3's "shifted-and-bridged ≈ 0 ⇒ defer" ruling is only as
    /// strong as this number is small.
    long long gap_unmeas_bridged{0};
    long long gap_unmeas_fresh{0};      ///< ARCS of an unmeasurable resumption that re-entered fresh (the safe case; closes the arc ledger).

    // --- Card G RESPONSE: network.cycle_slip.gap_reentry_relevel -------------
    //
    // Invariant #7, both edges.  The DECOUPLING edge is the re-entry itself
    // (already counted by the three RESUMPTION counters above); these count the
    // RE-COUPLING edge — what the response actually did to the state the break
    // was about to be bridged into.  Closure, in ARCS:
    //
    //   gap_reentry_arcs == gap_reentry_freed_shifted + gap_reentry_freed_lli
    //                     + gap_reentry_freed_unmeas + gap_reentry_no_relevel
    //                     + gap_reentry_dd_only + gap_reentry_cold_skipped
    //
    // ⚠ THE DENOMINATOR IS THE CARRY, NOT THE STATE (the wiring defect this
    // partition was re-cut for, 2026-09-09).  The first cut counted arcs with a
    // LIVE AMBIGUITY STATE, mirroring the MW/LLI drain.  For a RE-ENTRY that is
    // structurally the wrong question: an arc that left the model set had its
    // ambiguity pruned with the gap and PASS B's measurement pre-pass
    // (MeasurementModel::ensureRequiredStates) re-creates it only AFTER the slip
    // drains run — so on a 12 h field arm the drain saw 396 SLIPPED verdicts,
    // 775 signals with "no live ambiguity", and freed ZERO (byte-identical to
    // its control).  What actually survives the gap, and what the response
    // exists to stop from being re-injected, is the leveling CARRY entry — so
    // `gap_reentry_arcs` now counts arcs with something to PROTECT (a
    // @c LocalCarry entry still inside @c phase_leveling_max_gap_s, or the
    // carry_invariant DD vehicle) and @ref gap_reentry_no_carry counts the rest
    // (the inject would not have bridged those anyway).
    //
    // @ref gap_reentry_state_absent and @ref gap_reentry_dd_marked are both
    // ORTHOGONAL to the closure above — the first is a sub-count of
    // `gap_reentry_arcs` (how many were freed via a synthesised key while the
    // state was not yet re-created — the COMMON case by construction), the
    // second a different vehicle that can fire on the same arc.
    long long gap_reentry_verdicts{0};      ///< RESUMPTIONS the response judged SLIPPED (its population).
    long long gap_reentry_pivot_skipped{0}; ///< RESUMPTIONS at a gauge-pinned station index < i_lo (never refloated locally).
    long long gap_reentry_arcs{0};          ///< ARCS with a PROTECTABLE carry — a leveling entry inside the max-gap window, or the carry_invariant vehicle (the closure denominator).
    /// ARCS with NO carry to protect: no @c LocalCarry entry for
    /// (station, satellite, signal), or one whose gap already exceeds
    /// @c phase_leveling_max_gap_s so the PASS-B inject would skip it anyway.
    /// Deliberately OUTSIDE the closure: nothing was going to be bridged, so
    /// there is no response to make and none to report as missing.
    long long gap_reentry_no_carry{0};
    /// ARCS reached whose ambiguity STATE did not exist at drain time — freed
    /// via the synthesised @c StateKeys::makeAmbiguityKey the PASS-B inject
    /// site will use once the pre-pass re-creates the column.  This is the
    /// EXPECTED majority on a real re-entry (the state is pruned with the gap),
    /// which is exactly why the first cut's live-state walk was inert.  A
    /// pending-reseed mark is NOT set for these (a mark on an absent key would
    /// linger and be consumed by a later epoch's pre-pass — a response acting on
    /// history); the free-epoch latch is keyed on the string and needs no state.
    long long gap_reentry_state_absent{0};
    long long gap_reentry_freed_shifted{0}; ///< ARCS freed, primary cause = raw MW/GF level shift across the break.
    long long gap_reentry_freed_lli{0};     ///< ARCS freed, primary cause = RINEX LLI on the return epoch (level unshifted or unmeasurable).
    long long gap_reentry_freed_unmeas{0};  ///< ARCS freed, primary cause = UNMEASURABLE re-entry under the conservative sub-knob.
    /// ARCS not freed because the leveling carry is COLD (no warm value to be
    /// stale).  Diagnostic exactly as the MW/LLI and wind-up drains report it —
    /// a cold detection cannot act, and reporting only actions made those arms
    /// look dead (the Card-E L3 defect).
    long long gap_reentry_cold_skipped{0};
    /// ARCS that WOULD have been freed but @c cycle_slip.relevel_free_epoch is
    /// off, so the pending-reseed mark is consumed by the PASS-B pre-pass before
    /// the leveling inject reads it and the free epoch never lands
    /// (SPEC_slip_relevel_walk §6's root defect).  Counted, never silently
    /// no-op'd: this is the difference between "the response found nothing" and
    /// "the response could not act".
    long long gap_reentry_no_relevel{0};
    /// ARCS whose ONLY vehicle is the Card-4 C3c funnel: under
    /// @c network.carry_invariant the leveling local carry is superseded and
    /// structurally empty (the loader disarms @c relevel_free_epoch there for
    /// exactly that reason), so the funnel insert IS the whole response and
    /// nothing is missing.  Deliberately separate from @ref
    /// gap_reentry_no_relevel: one is by design, the other is a response that
    /// could not land.
    long long gap_reentry_dd_only{0};
    long long gap_reentry_dd_marked{0};     ///< AMBIGUITIES inserted into the Card-4 C3c invariant-carry funnel (carry_invariant vehicle; orthogonal to the buckets above).
    /// D5 innovation FLAG ROWS seen at the attribution site, one per RESOLVED
    /// flag.  Two things make this NOT a chord count, both deliberate:
    ///   * under the WL-3 joint pair block the gate runs per band ROW (rows i and
    ///     K+i are the two bands of chord i), so a chord flagged on both bands
    ///     appends the SAME (group, chord) twice — measured at 545 of 1 689 rows
    ///     on the 2026-07-22 150-station gate run, i.e. 32 % duplication;
    ///   * a flag whose chord does not resolve to an ambiguity in the reverse map
    ///     is skipped here (the producer warns once) but STILL gets a row in
    ///     network_d5_flags.csv with a blank amb_key.
    /// So dump rows = this counter + reverse-map misses, and the number of arcs
    /// the C3c funnel actually re-taps is @ref d5_retap_ambs.  Kept row-based for
    /// continuity with the 2026-07-21 hardening report's headline; the grader and
    /// the run-end line state both.
    long long d5_retaps{0};
    long long d5_retaps_stepped{0};     ///< …flag rows coincident with a raw step at the same (station, satellite, epoch) — the same evidence the dump's `raw_stepped` column reports.
    /// DISTINCT (station, ambiguity) pairs actually inserted into the C3c funnel
    /// this epoch — the TRUE re-tap count (one per arc that will re-tap fresh),
    /// free of the per-band double counting above.  Taken from the funnel set's
    /// own insert result, so it cannot drift from what the funnel does.
    long long d5_retap_ambs{0};
    long long d5_retap_ambs_stepped{0}; ///< …of those, coincident with a raw step at the same (station, satellite, epoch).

    // --- SPEC_slip_relevel_walk §6: the slip re-level response ---------------
    // relevel_free_epoch: (station, amb key) insertions into the latched
    // free-epoch set (the inject skip the consumed pending-reseed mark could
    // never deliver).  relevel_delta_bridge: the float 2×2 ladder — pairs
    // APPLIED (both bands shifted in place, arc keeps its level at phase
    // precision) and the fallback reasons (each falls back to the free epoch,
    // never to the pre-fix stale-injection path).  Invariant #4: either knob
    // armed with its headline counter at 0 on a slip-bearing run is a MISWIRE,
    // not a quiet run.
    long long relevel_free_marked{0};      ///< (station, amb key) free-epoch latches.
    // §10.4 wind-up restart response (network.cycle_slip.windup_restart_refloat).
    // Closure: wu_restart_events == wu_restart_refloated + wu_restart_cold
    //          + wu_restart_beyond_carry + wu_restart_stale.
    long long wu_restart_events{0};        ///< (station, sat) restart ledger entries drained.
    long long wu_restart_refloated{0};     ///< ...responded: arcs marked + free-epoch latched (carry warm, gap within the carry horizon).
    long long wu_restart_cold{0};          ///< ...carry not warm — diagnostic only (nothing stale to refloat yet).
    long long wu_restart_beyond_carry{0};  ///< ...gap exceeded the carry horizon — the carry died with the gap, no response needed.
    long long wu_restart_stale{0};         ///< ...ledger entry from an EARLIER epoch (station was absent at its restart) — refused, never respond to history.
    long long wu_restart_arcs{0};          ///< individual ambiguities refloated across all responses.
    long long relevel_free_skips{0};       ///< Inject-site skips actually taken (arc had a live carry).
    // §Z band-scoped DETECTOR refloat (network.cycle_slip.refloat_band_scope =
    // implicated; docs/SPEC_event_harness.md).  Armed with bscope_arcs = 0 on a
    // slip-bearing run is a MISWIRE (invariant #4) — every whole-satellite path
    // (IF-TD pair on a 2-band arc, LLI on every band, the wind-up restart drain)
    // reaches the scope.
    long long bscope_arcs{0};              ///< Arcs where the implicated set covered every live band and the scope ACTED.
    long long bscope_freed{0};             ///< Bands freed under those decisions.
    long long bscope_kept{0};              ///< Bands left pinned (one per acted arc by construction).
    long long bscope_measured{0};          ///< …of those, pinned on RAW MW/GF per-band evidence rather than the lexical fallback.
    long long bscope_all_band_fallback{0}; ///< …the honest cost line: a whole-satellite loss of lock where a truly-slipped band may stay pinned (see networkSlipBandScope).
    long long bscope_single_band{0};
    long long bscope_too_late{0};          ///< ...decisions where EVERY live band had already been freed by an earlier drain this epoch - nothing left to pin.  Nonzero means a freeing site upstream is not routed through the scope (invariant #4).       ///< Single-band arcs freed anyway — nothing to spare, and no LOS anchor to preserve.
    /// SPEC_withhold_era_release §8 (family D): leveling-carry entries erased by
    /// the DATUM-SLIP re-reference inside the MW refloat drain — the frame moved
    /// under the arc, so the carried value is stale in the old gauge.  Counted
    /// here rather than on NetworkServiceCounters because this drain already
    /// takes the SlipQcCounters by reference and threading an eighth optional
    /// sink through it would be worse than reading this one at teardown.
    long long datum_slip_carry_erases{0};
    long long bridge_pairs_applied{0};     ///< Delta-bridged (station, sat, pair) applications.
    long long bridge_joint{0};             ///< …with a same-epoch MW integer + IF-TD delta (the full 2×2).
    long long bridge_if_clean{0};          ///< …IF-TD flag + affirmatively-clean MW arc (ΔN_WL = 0 branch).
    long long bridge_mw_only{0};           ///< …MW integer + unflagged IF-TD delta (the IF-blind-ratio case).
    long long bridge_fallback_no_carry{0}; ///< Fallbacks: a band had no live carry entry (cold / pinned arc).
    long long bridge_fallback_reject{0};   ///< Fallbacks: kernel reject (magnitude / sigma / degenerate).
    long long bridge_fallback_refsat{0};   ///< Fallbacks: station's IF-TD refsat rebased this epoch (frame invalid).
    long long bridge_fallback_unpaired{0}; ///< Fallbacks: MW candidate never met an IF-TD delta.
    double    bridge_max_abs_dn_cyc{0};    ///< Largest |ΔN_band| ever applied (cycles).
    // Bridge v2 (pend-and-replace, SPEC §8.2):
    long long bridge_pends_created{0};        ///< Pends created at T1 flags (pre-slip capture + δIF).
    long long bridge_pend_expired{0};         ///< Pends that never met an MW integer inside the window.
    long long bridge_pend_voided{0};          ///< Pends voided by a fresh mark on the same (station, sat).
    long long bridge_replace_corrob_reject{0};///< Replaces rejected by the free-level corroboration gate.
    // Bridge v3 (the better second equation — GF row + EWL slot):
    long long bridge_gf_captured{0};          ///< Pends whose GF-TD row was captured at the flag epoch.
    long long bridge_gf_applied{0};           ///< Applies whose (1,2) plane came from the E1+E4 float route.
    long long bridge_plane_reject{0};         ///< E2-route kernel rejects tripped by the E4 plane residual (wrong WL integer screen).
    long long bridge_ewl_joint{0};            ///< EWL integers filled same-epoch at pend creation.
    long long bridge_ewl_belated{0};          ///< EWL integers filled belatedly in the MW drain.
    long long bridge_band3_applied{0};        ///< Applies that chained and replaced band 3.
};

/**
 * @brief The run-scope slip-QC hardening channel: one instance per
 *        runNetwork() invocation, threaded by reference into the PASS-A
 *        slip seam, the assembly pre-filter (trace) and the run-end summary.
 *
 * Bundling these five things keeps the seam signatures honest — the knobs, the
 * cross-epoch state they drive, this epoch's evidence, the wavelength provider
 * the evidence needs, and the counters that prove any of it ran, all travel
 * together.  A value copy anywhere on this path would silently reset the
 * per-station EWMAs every epoch, so it is deliberately non-copyable (the
 * unique_ptr member makes that a compile error, not a subtle regression).
 */
struct SlipQcChannel {
    SlipQcParams                params;    ///< Knob snapshot (taken once at run start).
    std::vector<RawSlipHistory> hist;      ///< Per-station cross-epoch state (sized once).
    RawSlipEpoch                epoch;     ///< THIS epoch's evidence (reset every epoch).
    SlipQcCounters              counters;  ///< Run-end telemetry (invariant #4).
    /// Wavelength provider for the raw observables.  NULL ⇒ no consumer asked
    /// for them this run ⇒ nothing is computed and every card is inert.
    std::unique_ptr<PppUtils>   utils;
    long long                   epoch_idx{0};  ///< Monotonic PROCESSED-epoch index.

    /// True when the raw observables must be computed this run.
    bool armed() const { return static_cast<bool>(utils); }
    /// True when a DECISION card (not merely the trace) is armed.
    bool cardsArmed() const { return params.lli_corroborate || params.gap_instrument || params.bridge_gf || params.iono_cs || params.retap_corr || params.lane_snap || params.gap_reentry; }
};

/**
 * @brief Compute this epoch's RAW slip observables for every present station.
 *
 * Byte-for-byte the arithmetic that used to sit inside the `trace_slip_raw`
 * writer: per present (station, satellite) pick the primary dual-frequency pair
 * (the two lowest DISTINCT bands whose phase AND matching code "C"+tail are
 * both present in `raw_observed`), then form MW, GF (= L1 − L2) and the
 * first-signal phase-minus-code, all in metres, plus the per-band RINEX LLI.
 * The speed of light cancels out of MW/GF, so wavelengths alone (FDMA-correct
 * via the satellite id) suffice.
 *
 * @param modeled      Per-station modeled observations (read-only).
 * @param present      Per-station PASS-A presence (absent stations produce no rows).
 * @param n_sta_total  Station count (the pivot IS included — diagnostic parity
 *                     with the original trace).
 * @param utils        Wavelength provider (non-const: PppUtils caches internally).
 * @param[out] out     Cleared and refilled; `out.rows[i]` follows `modeled[i]` order.
 */
void networkComputeRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  PppUtils& utils,
                                  RawSlipEpoch& out);

/**
 * @brief Adjudicate this epoch's raw steps, update the per-station empirical
 *        scales and the Card-L credibility state, and collect Card-G arc
 *        resumptions.  Serial, station-ordered → deterministic.
 *
 * Order of operations per station (the pool-arm pattern, deliberately):
 *   1. Form ΔMW / ΔGF against the IMMEDIATELY-adjacent processed epoch only.
 *   2. Threshold each satellite with the PRE-update scale (cold start: this
 *      epoch's own robust median), so the scale never absorbs the event it
 *      must catch.
 *   3. Fold the clean survivors' robust median into the EWMA.
 *   4. Fold this epoch's LLI-set fraction into the credibility EWMA and apply
 *      the two-state hysteresis.
 *   5. Record non-adjacent re-appearances as @ref GapResumption.
 *   6. Replace `prev` with this epoch's rows.
 *
 * @param modeled     Per-station modeled observations (LLI fraction denominator).
 * @param present     Per-station PASS-A presence.
 * @param n_sta_total Station count.
 * @param epoch_idx   Monotonic PROCESSED-epoch index (gap arithmetic unit).
 * @param p           Knob snapshot.
 * @param hist        Per-station cross-epoch state — MUTATED.
 * @param[in,out] ep  This epoch's buffer: `rows` read, everything else filled.
 * @param cnt         Run-end counters — accumulated into.
 */
void networkQualifyRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  long long epoch_idx,
                                  const SlipQcParams& p,
                                  std::vector<RawSlipHistory>& hist,
                                  RawSlipEpoch& ep,
                                  SlipQcCounters& cnt);

/**
 * @brief Robust scale of a sample of absolute steps: 1.4826·median(|x|).
 *
 * Exposed for unit tests and shared by the MW and GF arms.  Returns 0 for an
 * empty sample (the caller treats 0 as "no empirical information yet" and
 * falls back to the physical floor).
 */
double networkRobustStepScale(std::vector<double>& abs_steps);

/**
 * @brief Two-state LLI credibility transition with hysteresis.
 *
 * Pure function so the state machine is unit-testable without a station.
 * Never returns "ignore": the demoted state only means an assertion must be
 * corroborated to act.
 *
 * @param credible_in  Current state.
 * @param frac_ewma    Post-update EWMA of the LLI-set fraction.
 * @param count        Epochs folded into the EWMA.
 * @param p            Knobs (@ref SlipQcParams::chatter_frac_hi / _lo / _min_epochs).
 * @return The new state (== @p credible_in when the arming gate is not met).
 */
bool networkLliCredibilityStep(bool credible_in, double frac_ewma, long count,
                               const SlipQcParams& p);

} // namespace GnssProcessor

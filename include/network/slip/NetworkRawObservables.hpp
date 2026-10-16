#pragma once
/**
 * @file NetworkRawObservables.hpp
 * @brief Shared per-epoch raw cycle-slip observables (Melbourne-Wübbena,
 *        geometry-free, phase-minus-code, RINEX LLI) for the network run loop,
 *        and the empirical qualification pass whose verdicts cards L (LLI
 *        corroboration) and G (gap-coincident instrumentation) decide on.
 *
 * ## Why this seam exists
 *
 * The raw MW/GF arithmetic has one definition, computed here once per epoch
 * whenever any consumer needs it.  The consumers are the `network.trace_slip_raw`
 * writer (`ApplicationNetworkAssembly.cpp`) and the decision cards that use the
 * raw step as evidence rather than as a trace, among them the LLI-corroboration
 * gate (card L) and the arc-resumption instrument (card G);
 * @ref SlipQcParams::anyConsumer lists them all.  Computed inside the trace
 * writer, the numbers would be unavailable to every decision path and absent
 * whenever the trace is off; a copy in each drain would give several independent
 * definitions of "the raw step".  The trace writer is a pure consumer: when it is
 * the only consumer, its rows are byte-identical to those of the former inline
 * computation (docs/SPEC_slip_qc_hardening.md).
 *
 * ## Threading contract
 *
 * Both passes run serially in station order, so they are deterministic.  They are
 * called from the PASS-A slip scan (`Application::networkPassASlipScan_`) after
 * the `present[]` fold and before the drains and the observability pre-filter, so
 * a satellite that the pre-filter is about to prune is still measured, as the
 * original trace writer required.
 *
 * ## Empirical-threshold discipline (project rule 4, docs/SPEC_slip_qc_hardening.md §1)
 *
 * Every gate in this file derives from the observable stream itself: a robust
 * per-station scale of that station's own epoch-to-epoch steps (1.4826·median of
 * the clean survivors, folded into an EWMA), floored by a physical integer bound
 * kept in raw metres (by default half a wide-lane cycle for MW, 4 cm for GF).  No
 * stochastic-model σ enters a gate.  The IF-TD pool arm in NetworkSlipQc.cpp
 * follows the same empirical pattern.
 */

#include "common/GnssIdentity.hpp"       // LosId (RawSlipEpoch::gf_jump_flagged)
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "network/slip/NetworkGfArcJump.hpp"   // GfArcJumpParams / GfArcJumpArc / GfArcJumpCounters (card ARL)

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief The phase signal that station @p station latched on a (system, band digit)
 *        cell, as @c PppModel::latchedCanonicalPhaseOnBandDigit returns it (card GRK).
 *
 * The latch lives on each station's own @c PppModel and is locked for the run, so a
 * callable is used instead of a copied table: the call site binds one lambda over
 * the station vector, and the compute pass has no dependency on the model.  Under
 * @c network.roster_tracker (card SID phase 2) the call site binds the signal
 * roster's modelled phase instead, falling back to the station's latch where the
 * roster has no answer.  An empty return means the lookup does not know the cell:
 * the tracker then keeps its lexical pick, and the mismatch counter has no cell to
 * count.
 */
using TrackerLatchLookup =
    std::function<std::string(int station, const std::string& system, char band_digit)>;

/**
 * @brief One phase signal of a (station, satellite) at one epoch (card GRS).
 *
 * @ref RawSlipObs below summarizes a satellite through the two signals of its
 * primary pair and records nothing about the others.  The per-signal re-entry tier
 * needs the others: when a receiver loses lock on the semi-codeless L1/L2 phases
 * while the civil L5 keeps tracking, the satellite's row survives the dropout,
 * built on different signals, so the row-level tier sees no gap.  Filled only when
 * @c SlipQcParams::gap_reentry_per_signal is armed; with it off, nothing is
 * allocated or read.
 */
struct RawSigObs {
    std::string sig;        ///< Phase signal name ("L1C", "L5Q").
    double      phase_m{0.0}; ///< Phase in metres (cycles × the satellite-aware wavelength).
    double      code_m{0.0};  ///< The matching code observable "C"+tail (m).
    int         lli{0};       ///< RINEX LLI byte for @ref sig (bit 0 = loss of lock).
};

/**
 * @brief Raw slip observables for one (station, satellite) at one epoch.
 *
 * Absolute levels, not differences: the values the `network_slip_raw` trace
 * writes.  @ref networkQualifyRawObservables forms the epoch-to-epoch step against
 * the previous epoch's row for the same (station, satellite).
 *
 * The primary pair is the two lowest distinct bands whose phase and matching code
 * ("C" + signal tail) are both present.  When no distinct second band exists, or
 * its wavelength does not resolve, @ref dual is false and, of the combinations,
 * only @ref l1_c1_m is meaningful.
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
    /// γ−1 = (λ2/λ1)²−1, the dispersive scale of the pair; 0 when !dual.  Positive
    /// when sig1 is the higher-frequency band (every GPS pair); the CS-GF advance
    /// skips a row where it is not.
    double gf_denom{0.0};
    /// Wavelength of sig1 (m), set on every row, dual or not.  The LV-6 census
    /// converts to TECU with this per-pair value, since an assumed-L1 constant
    /// mis-scales the (L2, L5) fallback pair by 65 %.  Not traced.
    double lam1_m{0.0};
    double l1_c1_m{0.0};          ///< sig1 phase-minus-code (m); always meaningful.
    double lambda_wl_m{0.0};      ///< Wide-lane wavelength |1/(1/λ1 − 1/λ2)| (m); 0 when !dual.
    int    lli1{0};               ///< RINEX LLI byte for sig1 (bit 0 = loss of lock).
    int    lli2{0};               ///< RINEX LLI byte for sig2 (0 when !dual).
    /// Every phase signal with a matching code and a resolvable wavelength, in the
    /// lowest-band-first order the pair is picked from (card GRS).  Empty unless
    /// @c SlipQcParams::gap_reentry_per_signal is armed.
    std::vector<RawSigObs> sigs;
};

/**
 * @brief Carrier-smoothed GF-code slant-iono authority (CS-GF) for one line of
 *        sight (successor to QUEUE 8b; docs/SPEC_iono_carry_8b.md §6.2).
 *
 * A Hatch-class recursive estimate of the slant ionosphere, in metres at the
 * primary band and in the code channel's realization ι̃ = ι + d_r,GF − d^s_GF.
 * Between epochs the GF phase delta propagates the state (on an unslipped arc it
 * follows the ionospheric change exactly, because the ambiguities are constant),
 * and each epoch's GF code sample pulls the state toward the code channel with a
 * Kalman weight.  The estimate is level-independent by construction: the leveling
 * carry never touches it, and that is its purpose.  The solve-fed iono carry
 * accumulates the level-locked displacement (i1, §6.1), whereas this state
 * accumulates the code evidence without which the displacement never drains.  The
 * state resets when the qualify pass's empirical step gates trip (slip) and when
 * adjacency is lost (gap).  It recovers on its own after a reset and is not
 * snapshotted (the same exemption as @ref RawSlipHistory).
 */
struct IonoCsState {
    double val{0.0};   ///< Smoothed slant iono (m, primary band, code realization).
    /// Variance of @ref val (m²).  Formal: code multipath correlation makes it
    /// optimistic, and the inject floor covers the difference.
    double var{0.0};
    int    n{0};       ///< Samples since the last reset (0 = not initialized).
    /// The pair's primary-band wavelength (m), stamped at the advance.  The LV-6
    /// census converts @ref val with this pair's frequency, never an assumed L1.
    double lam1_m{0.0};
};

/// Per-carrier-step variance added by the GF-phase propagation
/// ((2σ_φ/denom)² class; conservative for the low-elevation tail).
inline constexpr double kIonoCsPhaseStepVar = 1e-4;   // m²
/// Zenith 1σ of one GF-code slant-iono sample: √2·σ_P0/denom at σ_P0 = 0.3 m and
/// the GPS L1/L2 denom 0.6469 (the seed-noise analysis' 0.66 m).  Elevation-mapped
/// by 1/sin at the call site.  A variance model for the estimator (stochastic-model
/// class), not a detector threshold, so the rule that MW thresholds are empirical
/// does not apply to it.
inline constexpr double kIonoCsCodeSigmaZenith = 0.66; // m

/**
 * @brief One CS-GF recursion step (pure, unit-testable).
 *
 * @param cs            The LOS's state (reset or advanced in place).
 * @param i_code_m      This epoch's GF-code slant-iono sample −gf_code/denom (m).
 * @param di_carrier_m  The adjacent-epoch GF-phase delta d_gf/denom (m);
 *                      meaningful only when @p adjacent.
 * @param adjacent      An immediately adjacent predecessor existed (verdict.measured).
 * @param stepped       The qualify pass's empirical MW/GF gate tripped (slip class).
 * @param sigma_i_code_m 1σ of @p i_code_m (elevation-mapped).
 * @return true when the call reset the state (gap, slip or first sighting).
 */
bool networkIonoCsUpdate(IonoCsState& cs, double i_code_m, double di_carrier_m,
                         bool adjacent, bool stepped, double sigma_i_code_m);

/**
 * @brief Adjudication of this epoch's raw step for one (station, satellite).
 *
 * Produced by @ref networkQualifyRawObservables against the immediately adjacent
 * previous processed epoch.  @ref measured is false when there is no adjacent
 * predecessor (first sighting, or a gap, which card G reports separately as a
 * @ref GapResumption) or when either row is single-band.  The step is then
 * unmeasurable and @ref stepped is false: an unmeasured step is evidence neither
 * of a slip nor of its absence.
 */
struct RawSlipVerdict {
    bool   measured{false};   ///< An adjacent-epoch predecessor row existed and both rows are dual (a step could be formed).
    bool   stepped{false};    ///< @ref mw_step || @ref gf_step.
    bool   mw_step{false};    ///< |ΔMW| exceeded the empirical MW gate.
    bool   gf_step{false};    ///< |ΔGF| exceeded the empirical GF gate.
    double d_mw{0.0};         ///< ΔMW across the epoch (m); 0 when !measured or !dual.
    double d_gf{0.0};         ///< ΔGF across the epoch (m); 0 when !measured or !dual.
    double gate_mw{0.0};      ///< Effective MW gate applied (m): max(k·scale, floor).
    double gate_gf{0.0};      ///< Effective GF gate applied (m).
    /// The adjacent predecessor used a different (sig1, sig2) pair, so @ref d_mw
    /// differences two MW combinations built on different signals (and, when a
    /// band changed, on different wide-lane wavelengths).  @ref measured stays
    /// true: the step exists, and the slip cards use it.  A consumer that converts
    /// @ref d_mw into cycles of a specific lane must refuse the row, because the
    /// step was not formed on that lane.
    bool   pair_changed{false};
};

/**
 * @brief One arc that reappeared after at least one missed processed epoch (card G).
 *
 * Missed epochs are counted in processed epochs (the run's own loop index), so a
 * decimated or start-offset run reports gaps in the units its estimator sees.
 * @ref shifted is the slip signature: the raw level moved across the break by more
 * than the empirical gate, so any state that bridged the break now carries a stale
 * value.
 */
struct GapResumption {
    int         station{-1};      ///< App station index.
    std::string satellite;        ///< Satellite id.
    std::string sig1, sig2;       ///< Primary pair at re-entry (sig2 empty when !dual).
    long long   gap_epochs{0};    ///< Processed epochs missed (≥ 1).
    bool        measurable{false};///< The pre-gap row and the return row both have a dual pair (a level shift could be seen).
    bool        shifted{false};   ///< |ΔMW| or |ΔGF| across the break exceeded the empirical gate.
    double      d_mw{0.0};        ///< MW level change across the break (m).
    double      d_gf{0.0};        ///< GF level change across the break (m).
    /// RINEX LLI byte for @ref sig1 on the return epoch (bit 0 = loss of lock).
    ///
    /// The receiver's own verdict about the break, and the only evidence a
    /// resumption has when @ref measurable is false (no dual-band level on both
    /// sides to difference).  Carried here rather than looked up again at the
    /// response site: the resumption was built from this raw row, and a second
    /// lookup in `modeled` would be a second definition of "the LLI of this arc at
    /// re-entry".  A slipped return can carry LLI on both bands while no MW or IF
    /// detector fires at that epoch (the DOY-129 worked case in
    /// docs/SPEC_slip_qc_hardening.md §12.1).
    int         lli1{0};
    int         lli2{0};          ///< …for @ref sig2 (0 when the return row is single-band).
};

/**
 * @brief One signal that reappeared after at least one missed processed epoch
 *        while its satellite's raw row survived the break (card GRS).
 *
 * The satellite-level @ref GapResumption cannot see this population: it keys on
 * the row, the row is rebuilt from whichever phases have a matching code, and a
 * satellite that keeps one signal through the dropout keeps its row.  The return
 * epoch then has an adjacent predecessor, no time-difference detector covers it
 * (MW needs an adjacent predecessor of the same pair, IF-TD three consecutive
 * epochs), and the carried ambiguity of the returning phase is folded across the
 * break.  Standing invariant #1 requires a response to that case.
 *
 * The level evidence is a geometry-free phase difference against a partner signal,
 * snapshotted at the candidate's own last-seen epoch.  When the partner tracked
 * through the break its ambiguity is constant across it, so d_gf isolates the
 * candidate's own step plus the ionospheric change over the gap.  The GF gate is
 * the station's per-epoch empirical scale; it does not grow with the gap length.
 */
struct SigResumption {
    int         station{-1};       ///< App station index.
    std::string satellite;         ///< Satellite id.
    std::string signal;            ///< The returning phase signal.
    std::string partner;           ///< Partner used for the level test (empty when unmeasurable).
    /// The partner's continuous run began at or before the candidate's last-seen
    /// epoch and reached the previous epoch, so it tracked through the gap.  Such a
    /// partner is the first-choice witness, and the only one whose ambiguity is
    /// known to be constant across the gap.
    bool        partner_through{false};
    long long   gap_epochs{0};     ///< Processed epochs missed (≥ 1).
    bool        measurable{false}; ///< A partner existed on both sides of the break (a level shift could be seen).
    bool        shifted{false};    ///< |d_gf| exceeded the station's empirical GF gate.
    double      d_gf{0.0};         ///< GF level change across the break (m); 0 when !measurable.
    double      gate_gf{0.0};      ///< The gate applied (m), max(k_gf·scale, gf_floor_m).
    int         lli{0};            ///< RINEX LLI byte for @ref signal on the return epoch.
};

/**
 * @brief Cross-epoch per-station state for the raw-observable instruments.
 *
 * One entry per app station, sized once at run start.  The serial qualification
 * pass mutates it; the per-station LLI accounting fields are incremented by the
 * MW drain's LLI qualification in NetworkSlipQc.cpp.  Not part of the RTB-7(a)
 * state snapshot: this is slip-QC scratch state that recovers within a few epochs
 * (the EWMAs re-converge, and `prev` refills on the next epoch), and a warm resume
 * that rebuilt it exactly would still see a one-epoch unmeasurable gap at the
 * resume boundary.  The exemption is recorded here rather than in
 * NetworkCarryState, so the snapshot's three-place rule stays intact.
 */
struct RawSlipHistory {
    /// The previous row for a satellite plus the processed-epoch index it came from.
    struct Prev {
        RawSlipObs obs;
        long long  epoch_idx{-1};
    };
    std::map<std::string, Prev> prev;   ///< Previous raw row per satellite.

    /**
     * @brief One signal's continuity record, per (satellite, signal) (card GRS).
     *
     * Each field answers one question of the test:
     *  - @ref last_epoch: did this signal miss an epoch?  The row's own adjacency
     *    cannot answer that, because the row survives on another pair;
     *  - @ref run_start: did this partner track through the gap?  This is the
     *    first-choice witness rule: a partner whose run began after the
     *    candidate's last-seen epoch has its own break inside the window, and its
     *    ambiguity is not known to be constant across it;
     *  - @ref gf_snap: the level to difference against, taken at the candidate's
     *    last-seen epoch (not at the gap's far side), so d_gf is a true
     *    across-the-break quantity.
     */
    struct SigPrev {
        long long last_epoch{-1};   ///< Last processed epoch at which this signal's phase (and matching code) was present.
        long long run_start{-1};    ///< Epoch at which the current continuous run of this signal began.
        double    phase_m{0.0};     ///< Phase (m) at @ref last_epoch.
        int       lli{0};           ///< RINEX LLI byte at @ref last_epoch.
        /// other signal → phase_m(this) − phase_m(other), at @ref last_epoch.
        std::map<std::string, double> gf_snap;
    };
    /// satellite → signal → continuity record.  Filled only when
    /// @c SlipQcParams::gap_reentry_per_signal and a row-level card (gap_instrument
    /// or gap_reentry) are armed; empty and untouched otherwise.
    std::map<std::string, std::map<std::string, SigPrev>> sig_prev;

    // --- Two-state LLI credibility (card L); neither state ignores LLI (see M1/M2) ---
    double    lli_frac_ewma{0.0};   ///< EWMA of the fraction of this station's tracked satellites asserting LLI.
    long      lli_frac_count{0};    ///< Epochs folded into the EWMA (hysteresis arming gate).
    bool      lli_credible{true};   ///< Two-state verdict: true ⇒ an uncorroborated LLI still acts.
    long long lli_demotions{0};     ///< credible → corroboration-required transitions over the run.
    long long lli_promotions{0};    ///< corroboration-required → credible transitions over the run.

    // --- Robust EWMA scales of this station's own raw steps, in metres (cards L, G) ---
    double mw_step_scale{0.0};      ///< EWMA of 1.4826·median(|ΔMW|) over clean satellites.
    long   mw_step_count{0};        ///< Clean samples folded (0 ⇒ use this epoch's own robust median).
    double gf_step_scale{0.0};      ///< EWMA of 1.4826·median(|ΔGF|) over clean satellites.
    long   gf_step_count{0};        ///< Clean samples folded.

    // --- Per-station LLI accounting for the top-offenders line (cards L, M4) ---
    long long lli_asserted{0};      ///< (satellite, epoch) LLI assertions seen.
    long long lli_acted{0};         ///< Assertions that drove a refloat / C3c funnel insert.
    long long lli_suppressed{0};    ///< Assertions suppressed as uncorroborated at a non-credible station.
    long long lli_corr_mw{0};       ///< Assertions corroborated by an MW step.
    long long lli_corr_gf{0};       ///< Assertions corroborated by a GF step (and not MW).
    long long lli_unmeasurable{0};  ///< Assertions with no measurable step (first sighting / gap / single-band).
    long long lli_offpair{0};       ///< Assertions whose LLI'd band sits outside the primary pair (unwitnessable by MW/GF).

    // --- Arc-scoped GF jump trigger (card ARL; network.cycle_slip.gf_arc_jump) ---
    /// satellite → that arc's state.  Mutated only by @c networkSlipDrainGfArcJump;
    /// empty and untouched while the trigger is off.
    std::map<std::string, GfArcJumpArc> gf_arcs;
    long long gf_jump_tests{0};     ///< Steps this station tested (the run-end per-station rate's denominator).
    long long gf_jump_flags{0};     ///< ...of those, flagged.
};

/**
 * @brief This epoch's raw observables and verdicts, as the drains and the run TU
 *        read them.  Cleared every epoch by @ref reset; nothing carries over.
 */
struct RawSlipEpoch {
    /// Per station, in `modeled[i]` order, one row per satellite that yielded a
    /// usable primary signal.  The trace writer walks this vector, so its rows are
    /// byte-identical to those of the former inline computation.
    std::vector<std::vector<RawSlipObs>> rows;
    /// Per station, satellite → this epoch's step adjudication.
    std::vector<std::map<std::string, RawSlipVerdict>> verdict;
    /// Per station, the card L credibility verdict used for this epoch's decisions;
    /// 1 = credible.  char rather than bool to avoid std::vector<bool>.
    std::vector<char> lli_credible;
    /// Per station, this epoch's raw LLI-set fraction (telemetry / log only).
    std::vector<double> lli_frac;
    /// Arcs that reappeared after ≥ 1 missed processed epoch (card G).
    std::vector<GapResumption> resumptions;
    /// Signals that reappeared after ≥ 1 missed processed epoch on a satellite
    /// whose row survived the break (card GRS).  Disjoint from @ref resumptions by
    /// construction: a satellite that produced a row-level resumption this epoch
    /// contributes nothing here, so no break is counted twice.
    std::vector<SigResumption> sig_resumptions;
    /// (station index, satellite) pairs whose raw level stepped this epoch: the
    /// cheap membership test the D5 attribution site needs (card G1c).
    std::set<std::pair<int, std::string>> stepped;
    /// The lines of sight the arc GF jump trigger flagged this epoch, whatever its
    /// action (refloat, coincident, cb_kept, no_arc, diagnostic, pivot).  Filled by
    /// @c networkSlipDrainGfArcJump; read by the producer AR search's suspect-corner
    /// stamp (card ARL, filter/NetworkArSuspect.hpp).  Empty while the trigger is off.
    std::set<LosId> gf_jump_flagged;
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
        sig_resumptions.clear();
        stepped.clear();
        gf_jump_flagged.clear();
        computed = false;
    }
};

/**
 * @brief Snapshot of the slip-QC knobs (cards L and G and the later cards that
 *        consume the raw observables), taken once per run.
 *
 * All defaults are inert.  Every master arm defaults off, and the two sub-gates
 * that default on (@ref gap_reentry_lli, @ref gap_reentry_per_signal_unmeasurable)
 * act only under their master arm.  The qualification pass is cheap and runs only
 * when a decision card is armed (@ref SlipQcChannel::cardsArmed); with every card
 * off, nothing is adjudicated.
 */
struct SlipQcParams {
    // --- LLI corroboration (card L; network.cycle_slip.mw_lli_*) ---
    bool   lli_corroborate{false};  ///< Master arm for the LLI qualification.
    double chatter_frac_hi{0.6};    ///< LLI-set fraction EWMA above which a station is demoted.
    double chatter_frac_lo{0.3};    ///< …and below which it is promoted back (hysteresis).
    int    chatter_min_epochs{1};   ///< Epochs of EWMA history before the verdict may change.
    double chatter_alpha{0.4};      ///< EWMA weight of the current epoch's fraction.
    double k_mw{5.0};               ///< MW step gate = max(k_mw·scale, floor_cyc·λ_WL).
    double k_gf{5.0};               ///< GF step gate = max(k_gf·scale, floor_m).
    double mw_floor_cyc{0.5};       ///< Physical MW floor in wide-lane cycles (audit's support test).
    double gf_floor_m{0.04};        ///< Physical GF floor in metres (audit's support test).
    double step_alpha{0.15};        ///< EWMA weight for the per-station step scales.

    // --- Arc-resumption instrument (card G; network.cycle_slip.gap_instrument) ---
    bool   gap_instrument{false};   ///< Master arm for the arc-resumption instrument.

    // --- Gap re-entry response (card G; network.cycle_slip.gap_reentry_relevel) ---
    /// Master arm for the gap re-entry slip response
    /// (docs/SPEC_slip_qc_hardening §G3/W1, §12).  Card G's decision rule calls
    /// for this response once `gap_shifted_bridged` is nonzero.
    ///
    /// The instrument and the response consume the same @ref GapResumption
    /// records, so this knob arms the resumption collection independently of
    /// @ref gap_instrument: the response does not require the diagnostic, and
    /// turning the diagnostic off does not leave the response inert.  The drain is
    /// `networkSlipDrainGapReentry` (slip/NetworkSlipQc.cpp).
    bool   gap_reentry{false};
    /// Sub-gate: a resumption whose return epoch asserts RINEX LLI on either band
    /// of the primary pair counts as slipped even when the raw MW/GF level did not
    /// clear the empirical gate.  On by default: LLI is the receiver's own
    /// loss-of-lock verdict, it is the only evidence an unmeasurable resumption can
    /// carry, and the field worked case (@ref GapResumption::lli1) asserts it with
    /// no MW or IF detection.
    bool   gap_reentry_lli{true};
    /// Sub-gate: treat an unmeasurable resumption (no dual-band level on both
    /// sides, so no level shift could be seen, and no LLI asserted) as slipped.
    /// Off by default: absence of evidence is not evidence of a slip, and on the
    /// field chain the unmeasurable-and-bridged population is the smaller one (24
    /// arcs, against 88 shifted-and-bridged).  Arming it is the conservative-reset
    /// posture.
    bool   gap_reentry_unmeasurable{false};

    /// True when the row-level resumption list (@ref RawSlipEpoch::resumptions) is
    /// collected: the instrument (@ref gap_instrument) or the response
    /// (@ref gap_reentry) is armed.  Both consume the same records, so either one arms
    /// the collection.  Read by the qualification pass, networkQualifyRawObservables
    /// (the resumption step, and the per-signal tier that de-duplicates against its
    /// list), and by the epoch driver's arc-resumption bridge census.
    bool collectsResumptions() const { return gap_instrument || gap_reentry; }

    // --- Per-signal gap re-entry (card GRS; network.cycle_slip.gap_reentry_per_signal) ---
    /// Rider of @ref gap_reentry: key the re-entry on the signal as well as on the
    /// row, so a loss of lock on the primary pair during which a third signal keeps
    /// the satellite present is still a re-entry.  Off by default; with it off, the
    /// run is byte-identical.  The per-signal response acts only with
    /// @ref gap_reentry armed (the loader warns otherwise), and the per-signal
    /// collection needs a row-level card (@ref gap_reentry or @ref gap_instrument):
    /// it de-duplicates against the row-level resumption list, and without that
    /// list one break would be counted by both tiers.
    bool   gap_reentry_per_signal{false};
    /// Sub-gate: treat a per-signal re-entry with no usable partner (nothing to
    /// difference the level against on both sides of the break) as slipped.  On by
    /// default, unlike the row-level @ref gap_reentry_unmeasurable: a partial
    /// dropout with no witness is the measured slip case (AB2 §10 to §12), and an
    /// arc whose satellite stayed present is the one whose carry survives the gap
    /// to be folded.
    bool   gap_reentry_per_signal_unmeasurable{true};
    /// Co-resumed share (network.cycle_slip.gap_reentry_coresumed_share): the
    /// per-signal resumptions of one (station, satellite) at one epoch are one
    /// re-acquisition, so a verdict on any of them frees every other co-resumed
    /// signal of that line of sight (cause `shared`).  A signal that tracked through
    /// the gap is not in the resumption list and is never freed by it.  The trigger
    /// may be any signal of the list, which holds every phase with a matching code,
    /// modelled or not (see @ref CoresumedShareLedger::los_trigger_uncarried).  Off by
    /// default; with it off, the run is byte-identical.  Acts only with
    /// @ref gap_reentry and @ref gap_reentry_per_signal (see
    /// @ref coresumedShareActive); it arms no collection of its own, since the
    /// per-signal list it reads is the rider's.
    bool   gap_reentry_coresumed_share{false};

    /// True when the co-resumed share can act: the knob and both tiers it rides on.
    bool coresumedShareActive() const {
        return gap_reentry_coresumed_share && gap_reentry && gap_reentry_per_signal;
    }

    // --- Latched tracker signal (card GRK; network.cycle_slip.tracker_modeled_first) ---
    /// Choose the arc-level tracker's signal on each band from the station's
    /// canonical phase latch (@ref TrackerLatchLookup) instead of lexically from
    /// @c raw_observed.  A band keeps the lexical pick when the latch has no entry
    /// for it, or when its latched signal lacks a raw phase and matching raw code
    /// this epoch.  Off by default; with it off, the pair, the trace rows and every
    /// detection are byte-identical.  Armed, the tracker's key matches the carried
    /// ambiguity key, so the gap re-entry response reaches the arc instead of
    /// counting @c gap_reentry_no_carry.  The per-signal tier (card GRS) is
    /// unaffected, since it keys on the signal.
    bool   tracker_modeled_first{false};

    // --- Ledger-driven re-entry (card SID phase 3; network.ledger_reentry) ---
    /// The gap re-entry response takes its population from the arc ledger's
    /// admitted lifecycle instead of from this tier's own resumption list, and
    /// looks up this tier's verdict per arc.  Off by default; with it off, both
    /// drains keep the raw-driven path byte for byte.  Armed, it is
    /// solve-affecting: the population is a different set (an arc can be
    /// raw-Tracking and admitted-Gap at the same epoch), so which carried levels
    /// survive a break changes.  The loader refuses it without
    /// @c network.roster_tracker and @c gap_reentry.
    bool   ledger_reentry{false};
    /// True when @c network.ledger_reentry_no_verdict is `refloat`: an arc in the
    /// ledger's population that no raw tier saw this epoch is treated as
    /// unmeasurable, and @ref gap_reentry_unmeasurable then decides.  Default false
    /// (`bridge`): the arc is left to the PASS-B inject and only counted and
    /// evented.  Inert without @ref ledger_reentry.
    bool   ledger_reentry_refloat{false};

    // --- GF-row capture, bridge v3 E4 (network.cycle_slip.relevel_bridge_gf) ---
    /// The GF-row capture consumes the qualified verdicts (d_gf and the empirical
    /// gf_step_scale), so it arms both the compute and the qualify pass.
    bool   bridge_gf{false};

    // --- CS-GF authority (network.level_authority_csgf) ---
    /// The carrier-smoothed GF-code slant-iono authority consumes the verdicts
    /// (d_gf carrier deltas + the empirical step gates as its slip reset), so
    /// it arms both passes too.
    bool   iono_cs{false};

    /// The arc-scoped GF jump trigger (card ARL; @c network.cycle_slip.gf_arc_jump*).
    /// It reads the raw rows only (its own per-arc state forms the steps), so it arms
    /// @ref anyConsumer and not @ref SlipQcChannel::cardsArmed.
    GfArcJumpParams gf_jump;

    /// True when any consumer needs the raw observables computed this run.
    bool anyConsumer(bool trace_slip_raw) const {
        return trace_slip_raw || lli_corroborate || gap_instrument || bridge_gf
            || iono_cs || gap_reentry
            || gap_reentry_per_signal || gf_jump.armed;
    }
};

/**
 * @brief Run-end tally for one IF-TD band pair (§14.10 of SPEC_event_harness).
 *
 * The on-path telemetry of `network.cycle_slip.if_phase_td_pairs` (standing
 * invariant #4).  Collected per pair kind ("L1C_L2W", "L1C_L5Q", …) whatever the
 * knob's value, so a `first` run already states how many samples its one watched
 * pair saw, and an `all` run shows whether the added pairs are live or inert: a
 * pair kind with a large @ref evaluated and @ref flagged at 0 over a slipped bed
 * indicates dead wiring, not a quiet lane.
 */
struct IfTdPairTally {
    long long evaluated{0};  ///< (station, satellite) samples thresholded on this pair.
    long long flagged{0};    ///< …of those, flagged by the pooled test.
    long long refloated{0};  ///< …of those, whose refloat actually freed ≥1 ambiguity (warm carry, not vetoed by the common-band scope, not oracle-handled).

    // --- Post-flag re-seed (card ARL; network.cycle_slip.if_phase_td_reseed_on_flag) ---
    // All zero while the knob is off.
    long long reseeded{0};             ///< Flags whose gradient was kept for the next epoch.
    long long reseed_nothing_kept{0};  ///< Flags with no flag-epoch gradient to keep (acted as a reset).
    long long post_flag_evaluated{0};  ///< Samples thresholded in the epoch after a re-seed (the evaluations a reset leaves out).
    long long post_flag_flagged{0};    ///< …of those, flagged: a slip repeated in the epoch after a flag (or a spike's return).
};

/// Pair label ("&lt;sig1&gt;_&lt;sig2&gt;") → tally; ordered, so the run-end line is deterministic.
using IfTdPairTallies = std::map<std::string, IfTdPairTally>;

/**
 * @brief Response ledger of one gap re-entry tier, in arcs (the re-coupling edge
 *        of invariant #7).
 *
 * The row-level tier keeps its flat fields in @ref SlipQcCounters (the run-end
 * card G line and the unit suite name them); the per-signal tier (card GRS) uses
 * this struct, so the two tiers are read side by side with the same partition.
 * The same code path fills both: @c networkSlipDrainGapReentry hands one of these
 * to the shared per-arc responder.  Closure, in arcs:
 *
 *   arcs == freed_shifted + freed_lli + freed_unmeas + no_relevel + dd_only
 *           + cold_skipped
 *
 * @ref no_carry is outside it (nothing was going to be bridged, so there is no
 * response to make), and @ref state_absent and @ref dd_marked are sub-counts of
 * @ref arcs, orthogonal to the closure buckets.
 */
struct GapReentryLedger {
    long long verdicts{0};      ///< Resumptions the response judged slipped (its population).
    long long pivot_skipped{0}; ///< Resumptions at a gauge-pinned station index < i_lo.
    long long arcs{0};          ///< Arcs with a protectable carry (the closure denominator).
    long long no_carry{0};      ///< Arcs with no carry to protect (outside the closure).
    long long state_absent{0};  ///< Arcs freed on a synthesised key while the column did not exist yet.
    long long freed_shifted{0}; ///< Arcs freed, primary cause = raw level shift across the break.
    long long freed_lli{0};     ///< Arcs freed, primary cause = RINEX LLI on the return epoch.
    long long freed_unmeas{0};  ///< Arcs freed, primary cause = unmeasurable re-entry.
    long long cold_skipped{0};  ///< Arcs not freed because the leveling carry is cold.
    long long no_relevel{0};    ///< Arcs that would have been freed but relevel_free_epoch is off.
    long long dd_only{0};       ///< Arcs answered by the C3c funnel alone (carry_invariant).
    long long dd_marked{0};     ///< Ambiguities fed into the C3c funnel (orthogonal).
};

/**
 * @brief Ledger of the per-signal tier's co-resumed share
 *        (network.cycle_slip.gap_reentry_coresumed_share), in signals and in lines
 *        of sight.
 *
 * Every count is 0 unless the share is active (@c SlipQcParams::coresumedShareActive).
 * A shared signal that reaches the responder is also counted in the tier's own
 * @ref GapReentryLedger (`verdicts`, `arcs` and the bucket its response landed in),
 * so the tier's closure is unchanged; the buckets below are the share's sub-counts
 * of those, one for one, so the GRS line can be reconciled from the run end alone:
 * GRS `freed_shifted` = the tier's own shifted frees + @ref freed_shifted here, and
 * likewise for `freed_lli`, `freed_unmeas`, `cold_skipped`, `no_relevel`, `dd_only`
 * and `no_carry`.  Closure, in signals:
 *
 *   signals == already_freed + skipped + no_carry + freed_shifted + freed_lli
 *              + freed_unmeas + cold_skipped + no_relevel + dd_only
 *
 * The freed buckets are split by the TRIGGER's primary cause: a shared signal's own
 * test read clean, so the cause is the sibling's.  @ref already_freed and
 * @ref skipped are outside the tier's `verdicts`/`arcs` (an already-freed signal is
 * given no verdict; a skipped one is given a verdict and no response, as the tier's
 * own skips are).
 *
 * Invariant #4: armed, @ref los_coresumed above 0 with @ref signals at 0 says that
 * every co-resumed group was uniformly clean or uniformly judged, a statement about
 * the data; @ref los_coresumed at 0 says there was nothing for the share to read.
 */
struct CoresumedShareLedger {
    /// (station, satellite) re-acquisitions with at least two co-resumed signals at a
    /// station the tier answers (in range, present, not gauge-pinned): the population
    /// the share can act on, in lines of sight.
    long long los_coresumed{0};
    /// …at which the share passed at least one co-resumed signal to the responder.
    long long los_answered{0};
    /// …of @ref los_answered, those whose trigger had no leveling carry entry inside
    /// the max-gap window: a signal the station does not carry (unmodelled, such as
    /// L5Q or L2L on an L1/L2 posture) or one with nothing carried.  Under the
    /// carry_invariant vehicle the leveling carry is empty, so every trigger counts.
    long long los_trigger_uncarried{0};
    /// Co-resumed signals with no verdict of their own whose line of sight has a
    /// judged sibling (the share's population, in signals).
    long long signals{0};
    /// …already in this epoch's free set (an earlier drain, such as the MW drain,
    /// freed the same ambiguity): given no verdict and no second response.
    long long already_freed{0};
    /// …given the shared verdict and no response: at a gauge-pinned station, or,
    /// under network.ledger_reentry, outside the ledger's admitted population.
    long long skipped{0};
    long long no_carry{0};      ///< …responded to with no carry to protect.
    long long freed_shifted{0}; ///< …freed (refloat), trigger's primary cause = level shift.
    long long freed_lli{0};     ///< …freed (refloat), trigger's primary cause = RINEX LLI.
    long long freed_unmeas{0};  ///< …freed (refloat), trigger's primary cause = unmeasurable.
    long long cold_skipped{0};  ///< …reached, not freed: the leveling carry is cold.
    long long no_relevel{0};    ///< …reached, not freed: relevel_free_epoch is off.
    long long dd_only{0};       ///< …reached, answered by the C3c funnel alone (carry_invariant).
};

/**
 * @brief Run-end counters for the slip-QC hardening cards (standing invariant
 *        #4: inert wiring is visible only through counters).
 *
 * Card E counts detections separately from refloats: a cold run detects and does
 * not refloat, and a count of refloats alone makes the MW/LLI arm look dead
 * (defect L3, fixed by card E).
 */
struct SlipQcCounters {
    // --- MW-detect and LLI detections against actions (card E) ---
    long long mw_detections{0};     ///< (station, satellite) wide-lane slip detections.
    long long mw_refloats{0};       ///< …of those, warm epochs that actually refloated.
    long long lli_detections{0};    ///< (station, satellite) LLI detections that acted (after the card L qualification).
    long long lli_refloats{0};      ///< …of those, warm epochs that actually refloated.

    // --- Datum-station slip detect (network.cycle_slip.datum_station_detect) ---
    // Kept separate from the card E populations above: those count non-pivot
    // detections, whose action is a local refloat; these count the gauge-pinned
    // datum station, whose action is a network re-reference.
    long long datum_slip_detections{0}; ///< (datum station, satellite) MW/LLI slip detections (queue edge).
    long long datum_slip_rerefs{0};     ///< …network re-references applied (pb reseed + all-station amb reseed + disc mark).

    // --- LLI qualification (card L) ---
    long long lli_asserted{0};      ///< Raw (station, satellite) LLI assertions seen.
    long long lli_suppressed{0};    ///< Suppressed: uncorroborated at a non-credible station.
    long long lli_corr_mw{0};       ///< Corroborated by an MW step.
    long long lli_corr_gf{0};       ///< Corroborated by a GF step (MW quiet or unavailable).
    long long lli_unmeasurable{0};  ///< No measurable step (first sighting / gap / single band).
    /// Assertions whose LLI'd band is outside the primary dual-frequency pair the
    /// shared observables are formed on, so MW/GF cannot witness them.  A nonzero
    /// count calls for widening the corroboration primitive to every tracked pair,
    /// not for distrusting it.
    long long lli_offpair{0};
    long long lli_credible_epochs{0};   ///< (station, epoch) pairs judged credible.
    long long lli_demoted_epochs{0};    ///< (station, epoch) pairs judged corroboration-required.

    // --- Arc resumptions across a tracking break (card G) ---
    //
    // Units (spec-followups N3/H2, docs/SPEC_slip_qc_hardening.md §9).  The two
    // units are not interchangeable, and the summary line keeps them apart:
    //   * the first three counters count resumptions: one per (station, satellite)
    //     re-entry, the unit the raw MW/GF level shift is measured in;
    //   * the six bridge-classification counters count arcs: one per (station,
    //     satellite, signal), because carry chords are per arc, so a dual-band
    //     satellite returning once contributes two independently bridgeable arcs.
    // So `gap_shifted` (resumptions) is smaller than
    // `gap_shifted_bridged + gap_shifted_fresh` (arcs) by construction; on dual-band
    // data the arc count is about twice the resumption count, which is not a lost
    // count.  The arc ledger mirrors the resumption three-way split into shifted,
    // clean (measured and unshifted) and unmeasurable, each closed as bridged +
    // fresh, so every arc is counted once and an unmeasurable resumption is never
    // reported as clean.
    long long gap_resumptions{0};       ///< Resumptions: arcs re-established after ≥ 1 missed processed epoch.
    long long gap_unmeasurable{0};      ///< Resumptions: …with no dual-band level on both sides of the break to compare.
    long long gap_shifted{0};           ///< Resumptions: …whose raw MW/GF level shifted beyond the empirical gate.
    long long gap_shifted_bridged{0};   ///< Arcs of a shifted resumption that re-entered with bridged carry state (the harmful case).
    long long gap_shifted_fresh{0};     ///< Arcs of a shifted resumption that re-entered as a fresh float (benign).
    long long gap_clean_bridged{0};     ///< Arcs of a measured-and-unshifted resumption that re-entered bridged (the intended behaviour).
    long long gap_clean_fresh{0};       ///< Arcs of a measured-and-unshifted resumption that re-entered fresh (a conservative reset).
    /// Arcs of an unmeasurable resumption (no dual-band level on both sides) that
    /// re-entered bridged.  Not folded into @ref gap_clean_bridged: state that
    /// bridged a break that could not be checked is unknown, not clean, and G3's
    /// "shifted-and-bridged ≈ 0 ⇒ defer" ruling is only as strong as this number
    /// is small.
    long long gap_unmeas_bridged{0};
    long long gap_unmeas_fresh{0};      ///< Arcs of an unmeasurable resumption that re-entered fresh (the safe case; closes the arc ledger).

    // --- Gap re-entry response (card G; network.cycle_slip.gap_reentry_relevel) ---
    //
    // Invariant #7, both edges.  The decoupling edge is the re-entry itself,
    // counted by the three resumption counters above; the counters below count the
    // re-coupling edge: what the response did to the state the break was about to
    // be bridged into.  Closure, in arcs:
    //
    //   gap_reentry_arcs == gap_reentry_freed_shifted + gap_reentry_freed_lli
    //                     + gap_reentry_freed_unmeas + gap_reentry_no_relevel
    //                     + gap_reentry_dd_only + gap_reentry_cold_skipped
    //
    // The denominator counts carry entries, not live ambiguity states.  An arc that
    // left the model set had its ambiguity pruned with the gap, and PASS B's
    // measurement pre-pass (MeasurementModel::ensureRequiredStates) re-creates it
    // only after the slip drains run, so at drain time a returning arc usually has
    // no ambiguity state.  What survives the gap, and what the response keeps from
    // being re-injected, is the leveling carry entry.  `gap_reentry_arcs` therefore
    // counts arcs with something to protect (a @c LocalCarry entry still inside
    // @c phase_leveling_max_gap_s, or the carry_invariant DD vehicle), and
    // @ref gap_reentry_no_carry counts the rest (the inject would not bridge those
    // anyway).  See docs/SPEC_slip_qc_hardening.md §12.3.
    //
    // @ref gap_reentry_state_absent and @ref gap_reentry_dd_marked are both
    // orthogonal to the closure above: the first is a sub-count of
    // `gap_reentry_arcs` (arcs freed through a synthesised key while the state was
    // not yet re-created, the common case by construction), the second a different
    // vehicle that can act on the same arc.
    long long gap_reentry_verdicts{0};      ///< Resumptions the response judged slipped (its population).
    long long gap_reentry_pivot_skipped{0}; ///< Resumptions at a gauge-pinned station index < i_lo (never refloated locally).
    /// Arcs with a protectable carry: a leveling entry inside the max-gap window, or
    /// the carry_invariant vehicle (the closure denominator).
    long long gap_reentry_arcs{0};
    /// Arcs with no carry to protect: no @c LocalCarry entry for
    /// (station, satellite, signal), or one whose gap already exceeds
    /// @c phase_leveling_max_gap_s, so the PASS-B inject would skip it anyway.
    /// Outside the closure: nothing was going to be bridged, so there is no
    /// response to make and none to report as missing.
    long long gap_reentry_no_carry{0};
    /// The key-mismatch part of @ref gap_reentry_no_carry (card GRK).  Row-level
    /// re-entry arcs (the unit of @c networkSlipDrainGapReentry) that found no carry
    /// under the tracker's band signal while a @c LocalCarry entry for the same
    /// (station, satellite) existed on another signal of the same band, still
    /// inside @c phase_leveling_max_gap_s, and that other signal is present at this
    /// epoch as a phase with its matching code.  Counted in both states of
    /// @c network.cycle_slip.tracker_modeled_first, so the knob's target population
    /// is visible with the knob off and on (standing invariant #4).
    ///
    /// Two restrictions prevent over-counting:
    ///  - the carried signal must be tracked this epoch.  Otherwise the count
    ///    includes arcs whose carried signal is absent from the record: a station
    ///    that reports C1C, C1L, L1L, C2L, L2L, C5Q and L5Q but no L1C, L2W or C2W
    ///    has the tracker fall back, correctly, to L1L / L2L, and the carried arcs
    ///    have no row at all;
    ///  - only the row-level tier feeds it.  The per-signal tier (card GRS) keys on
    ///    the returning signal, so a key mismatch cannot occur there; without the
    ///    exclusion, a per-signal return on a signal the station does not model
    ///    would be scored against the modelled same-band carry that tracked through
    ///    the break.
    ///
    /// The presence test reads the raw row's per-signal list, which exists only
    /// under @c gap_reentry_per_signal.  Without the list the responder receives no
    /// signal set, and the counter keeps the broader definition (a carry on the
    /// same band, inside the window) rather than reading a silent zero.
    long long gap_reentry_unreachable{0};
    /// Extra-channel arcs the row-level gap re-entry tier responded to beyond the
    /// tracker's one phase per band, summed over the run (card SID phase 6b, R3).
    /// The tier keys on the satellite and frees the tracker's two signals; under
    /// `processing.channel_mode: all` the station also carries every other admitted
    /// phase attribute of those bands, and a whole-row gap interrupts them too.  The
    /// per-signal tier does not reach them either (it skips every satellite this
    /// tier already owns), so a zero here on an all-channel run means the response
    /// never fired.  Structurally 0 on every one-channel-per-band posture, where the
    /// extra set is empty.
    long long gap_reentry_extra_channel{0};
    /// The decoupling half of the key mismatch (card GRK): (station, satellite,
    /// epoch, band) cells at which the arc-level tracker's chosen signal differs
    /// from the phase this station latched on that band, counted only where the
    /// latch names a signal that is also tracked with its matching code this epoch.
    /// Telemetry of the mismatch itself, independent of the knob: an off run states
    /// the size of the population, and an armed run reads 0 by construction.
    long long gap_tracker_key_mismatch{0};
    /// Arcs reached whose ambiguity state did not exist at drain time, freed through
    /// the synthesised @c StateKeys::makeAmbiguityKey that the PASS-B inject site
    /// uses once the pre-pass re-creates the column.  This is the expected majority
    /// on a real re-entry, because the state is pruned with the gap; a walk over
    /// live states alone reaches none of these arcs.  No pending-reseed mark is set
    /// for them: a mark on an absent key would linger and be consumed by a later
    /// epoch's pre-pass, a response acting on history.  The free-epoch latch is
    /// keyed on the string and needs no state.
    long long gap_reentry_state_absent{0};
    long long gap_reentry_freed_shifted{0}; ///< Arcs freed, primary cause = raw MW/GF level shift across the break.
    long long gap_reentry_freed_lli{0};     ///< Arcs freed, primary cause = RINEX LLI on the return epoch (level unshifted or unmeasurable).
    long long gap_reentry_freed_unmeas{0};  ///< Arcs freed, primary cause = unmeasurable re-entry under the conservative sub-knob.
    /// Arcs not freed because the leveling carry is cold (no warm value to be
    /// stale).  A diagnostic, reported the same way as by the MW/LLI and wind-up
    /// drains: a cold detection cannot act, and a count of actions alone makes
    /// those arms look dead (defect L3, card E).
    long long gap_reentry_cold_skipped{0};
    /// Arcs that would have been freed but @c cycle_slip.relevel_free_epoch is
    /// off: the pending-reseed mark is then consumed by the PASS-B pre-pass before
    /// the leveling inject reads it, and the free epoch never lands
    /// (SPEC_slip_relevel_walk §6, root defect).  Counted rather than dropped
    /// silently, so "the response found nothing" and "the response could not act"
    /// stay distinguishable.
    long long gap_reentry_no_relevel{0};
    /// Arcs whose only vehicle is the Card-4 C3c funnel: under
    /// @c network.carry_invariant the leveling local carry is superseded and
    /// structurally empty (which is why the loader disarms @c relevel_free_epoch
    /// there), so the funnel insert is the whole response and nothing is missing.
    /// Kept separate from @ref gap_reentry_no_relevel: this case is by design, the
    /// other is a response that could not land.
    long long gap_reentry_dd_only{0};
    long long gap_reentry_dd_marked{0};     ///< Ambiguities inserted into the Card-4 C3c invariant-carry funnel (carry_invariant vehicle; orthogonal to the buckets above).

    // --- Per-signal re-entry tier (card GRS) ---------------------------------
    //
    // The decoupling edge (invariant #7): the first three counters below count
    // per-(station, satellite, signal) re-entries that the row-level tier cannot
    // see.  They are disjoint from the row-level `gap_*` populations: a satellite
    // that produced a row-level resumption this epoch contributes no per-signal
    // one.  Invariant #4: armed, with @ref gap_sig_resumptions at 0 on a field run
    // with third-band tracking, the tier is dead wiring, not a quiet run.
    long long gap_sig_resumptions{0};   ///< Signal re-entries seen (the tier's population).
    long long gap_sig_shifted{0};       ///< …whose GF level against the partner moved beyond the empirical gate.
    long long gap_sig_unmeasurable{0};  ///< …with no usable partner on both sides of the break.
    /// …measured with a second-choice witness: no partner tracked through the gap,
    /// so the level test also carries the partner's own re-entry, and a `shifted`
    /// verdict names the pair rather than this signal.  A diagnostic sub-count of
    /// @ref gap_sig_resumptions, never a gate.
    long long gap_sig_partner_broken{0};
    /// The re-coupling edge: what the response did to the state the break was about
    /// to be bridged into (see @ref GapReentryLedger for the closure).
    GapReentryLedger gap_sig;

    /// The per-signal tier's co-resumed share
    /// (network.cycle_slip.gap_reentry_coresumed_share), sub-counts of
    /// @ref gap_sig; all 0 unless the share is active (see
    /// @ref CoresumedShareLedger for the closure).
    CoresumedShareLedger gap_sig_share;

    // --- Ledger-driven re-entry population (card SID phase 3) -----------------
    //
    // Every counter here is zero unless @c SlipQcParams::ledger_reentry is armed,
    // and each is stated against a denominator so a zero can be read (standing
    // invariant 4): @ref ledger_reentry_arcs is the population, and the four
    // dispositions @ref ledger_reentry_freed, @ref ledger_reentry_bridged,
    // @ref ledger_reentry_no_carry and @ref ledger_reentry_pivot_skipped partition
    // it (the run-end line prints their sum against the population).
    long long ledger_reentry_arcs{0};         ///< Arcs in the ledger's admitted Gap-to-Reentered population, inside the carry horizon (the denominator).
    long long ledger_reentry_with_verdict{0}; ///< …for which a raw tier had a verdict this epoch.
    long long ledger_reentry_v_shifted{0};    ///< …of those, primary verdict = raw MW/GF level shift.
    long long ledger_reentry_v_lli{0};        ///< …of those, primary verdict = RINEX LLI at the return epoch.
    long long ledger_reentry_v_unmeas{0};     ///< …of those, primary verdict = unmeasurable under the tier's sub-knob.
    long long ledger_reentry_v_clean{0};      ///< …of those, the tier saw the return and called it clean (bridged by verdict).
    /// …of @ref ledger_reentry_v_shifted, @ref ledger_reentry_v_lli and
    /// @ref ledger_reentry_v_unmeas, the arcs whose own per-signal verdict was clean
    /// and that took a co-resumed sibling's verdict
    /// (network.cycle_slip.gap_reentry_coresumed_share; an arc already in this
    /// epoch's free set keeps its clean verdict).  Counted in the bucket of
    /// the sibling's primary cause, so the verdict partition is unchanged; 0 unless
    /// the share is active.
    long long ledger_reentry_v_shared{0};
    long long ledger_reentry_no_verdict{0};   ///< …that no raw tier saw this epoch (raw Tracking while the admitted side gapped).
    long long ledger_reentry_freed{0};        ///< Arcs the response actually reached (a carry existed and the mark landed).
    long long ledger_reentry_bridged{0};      ///< Arcs deliberately left to the PASS-B inject (clean verdict, or no verdict under `bridge`).
    long long ledger_reentry_no_carry{0};     ///< Arcs with a verdict and a response, but nothing to protect.
    long long ledger_reentry_outside_horizon{0}; ///< Admitted returns past phase_leveling_max_gap_s, outside the population.
    /// Admitted phase rows whose signal the roster does not call Modelled; never
    /// offered to the population.  Dominated by the first epoch of each
    /// constellation, whose latch is decided downstream of the roster build.
    long long ledger_reentry_not_modelled{0};
    long long ledger_reentry_row_tier{0};     ///< …of @ref ledger_reentry_with_verdict, answered from the row-level tier.
    long long ledger_reentry_sig_tier{0};     ///< …of @ref ledger_reentry_with_verdict, answered from the per-signal tier.
    long long ledger_reentry_pivot_skipped{0};///< Arcs at a gauge-pinned station index below i_lo (never refloated locally).
    long long ledger_reentry_state_recreated{0}; ///< Arcs whose ambiguity column had been pruned inside the gap they return from.
    long long ledger_reentry_events{0};       ///< `ARC` / `arc_reentry` event rows written for the no-verdict population (holdoff-throttled).
    /// The no-verdict population of @ref ledger_reentry_no_verdict, split by the
    /// reason mask of the admitted gap that preceded the return (card SID phase 4a,
    /// S1).  One entry per reason bit index (@c arcReasonIndex);
    /// @ref ledger_reentry_nv_no_mask counts the returns whose gap carries no mask
    /// (the gap opened before the writers were armed, or the fold did not run for
    /// it).  Zero unless network.ledger_screens is armed.  This split is the
    /// attribution owed to the population that phase 3 named a screen-side
    /// decoupling.
    std::vector<long long> ledger_reentry_nv_reason;
    long long ledger_reentry_nv_no_mask{0};
    /// Flag rows of the D5 innovation gate seen at the attribution site, one per
    /// resolved flag (a flag whose chord does not resolve to an ambiguity in the
    /// reverse map is skipped here; the producer warns once).  The number of arcs the
    /// C3c funnel actually re-taps is @ref d5_retap_ambs.  The counter stays
    /// row-based for continuity with the headline of the hardening report
    /// (docs/SPEC_slip_qc_hardening.md §8); the grader and the run-end line state
    /// both.
    long long d5_retaps{0};
    long long d5_retaps_stepped{0};     ///< …flag rows coincident with a raw step at the same (station, satellite, epoch); the same evidence the v3 corroboration gate reads.
    /// Distinct (station, ambiguity) pairs inserted into the C3c funnel at each
    /// epoch, summed over the run: the true re-tap count (one per arc that will
    /// re-tap fresh), de-duplicated per (station, ambiguity) and epoch.  Taken from the
    /// funnel set's own insert result, so it cannot drift from what the funnel does.
    long long d5_retap_ambs{0};
    long long d5_retap_ambs_stepped{0}; ///< …of those, coincident with a raw step at the same (station, satellite, epoch).

    // --- Slip re-level response, SPEC_slip_relevel_walk §6 --------------------
    // relevel_free_epoch: (station, amb key) insertions into the latched free-epoch
    // set (the inject skip that the consumed pending-reseed mark cannot deliver).
    // relevel_delta_bridge: the float 2×2 ladder, counting pairs applied (both bands
    // shifted in place, so the arc keeps its level at phase precision) and the
    // fallback reasons (each falls back to the free epoch, never to injecting the
    // stale value).  Invariant #4: either knob armed with its headline counter at 0
    // on a slip-bearing run indicates a miswire, not a quiet run.
    long long relevel_free_marked{0};      ///< (station, amb key) free-epoch latches.
    // Wind-up restart response (network.cycle_slip.windup_restart_refloat;
    // SPEC_phase_windup_sign §10.4, built in §11).
    // Closure: wu_restart_events == wu_restart_refloated + wu_restart_no_arc
    //          + wu_restart_cold + wu_restart_beyond_carry + wu_restart_stale.
    long long wu_restart_events{0};        ///< (station, sat) restart ledger entries drained.
    long long wu_restart_refloated{0};     ///< ...responded: at least one arc marked + free-epoch latched (carry warm, gap within the carry horizon).
    long long wu_restart_no_arc{0};        ///< ...warm and inside the carry horizon with nothing to free: no live column and no carry entry in the horizon (card WUP; before it these were counted as responded).
    long long wu_restart_cold{0};          ///< ...carry not warm: diagnostic only (nothing stale to refloat yet).
    long long wu_restart_beyond_carry{0};  ///< ...gap exceeded the carry horizon: the carry ended with the gap, so no response is needed.
    long long wu_restart_stale{0};         ///< ...ledger entry from an earlier epoch (the station was absent at its restart): refused, since a response must not act on history.
    long long wu_restart_arcs{0};          ///< individual ambiguities refloated across all responses.
    long long wu_restart_carry_arcs{0};    ///< ...of those, arcs reached through the leveling carry (a re-entering arc with no live column; card WUP).
    long long relevel_free_skips{0};       ///< Inject-site skips actually taken (arc had a live carry).
    /// Leveling-carry entries erased by the datum-slip re-reference inside the MW
    /// refloat drain: the frame moved under the arc, so the carried value is stale
    /// in the old gauge (SPEC_withhold_era_release §8, family D).  Counted here
    /// rather than on NetworkServiceCounters because the drain already takes
    /// SlipQcCounters by reference; reading this counter at teardown is simpler
    /// than threading an eighth optional sink through the drain.
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
    // Bridge v2 (pend-and-replace, SPEC_slip_relevel_walk §8.2):
    long long bridge_pends_created{0};        ///< Pends created at T1 flags (pre-slip capture + δIF).
    long long bridge_pend_expired{0};         ///< Pends that never met an MW integer inside the window.
    long long bridge_pend_voided{0};          ///< Pends voided by a fresh mark on the same (station, sat).
    long long bridge_replace_corrob_reject{0};///< Replaces rejected by the free-level corroboration gate.
    // Bridge v3 (second equation from the GF row; SPEC_slip_relevel_walk §8.3):
    long long bridge_gf_captured{0};          ///< Pends whose GF-TD row was captured at the flag epoch.
    long long bridge_gf_applied{0};           ///< Applies whose (1,2) plane came from the E1+E4 float route.
    long long bridge_plane_reject{0};         ///< E2-route kernel rejects tripped by the E4 plane residual (wrong WL integer screen).

    // --- Arc-scoped GF jump trigger (card ARL; network.cycle_slip.gf_arc_jump) ---
    GfArcJumpCounters gf_jump;                ///< Tests, flags and the flag closure (networkGfArcJumpRunEnd).
};

/**
 * @brief The run-scope slip-QC hardening channel: one instance per
 *        runNetwork() invocation, passed by reference to the PASS-A slip seam,
 *        the assembly pre-filter (trace) and the run-end summary.
 *
 * Five parts travel together, so every seam signature carries all of them: the
 * knobs, the cross-epoch state they drive, this epoch's evidence, the wavelength
 * provider the evidence needs, and the counters that show any of it ran.  A value
 * copy anywhere on this path would silently reset the per-station EWMAs every
 * epoch, so the struct is non-copyable: the unique_ptr member makes a copy a
 * compile error rather than a silent regression.
 */
struct SlipQcChannel {
    SlipQcParams                params;    ///< Knob snapshot (taken once at run start).
    std::vector<RawSlipHistory> hist;      ///< Per-station cross-epoch state (sized once).
    RawSlipEpoch                epoch;     ///< This epoch's evidence (reset every epoch).
    SlipQcCounters              counters;  ///< Run-end telemetry (invariant #4).
    /// Wavelength provider for the raw observables.  Null when no consumer asked
    /// for them this run; nothing is then computed and every card is inert.
    std::unique_ptr<PppUtils>   utils;
    long long                   epoch_idx{0};  ///< Monotonic processed-epoch index.

    /// True when the raw observables must be computed this run.
    bool armed() const { return static_cast<bool>(utils); }
    /// True when a decision card (not only the trace) is armed.
    bool cardsArmed() const { return params.lli_corroborate || params.gap_instrument || params.bridge_gf || params.iono_cs || params.gap_reentry || params.gap_reentry_per_signal; }
};

/**
 * @brief Compute this epoch's raw slip observables for every present station.
 *
 * The arithmetic is byte-for-byte that of the former inline `trace_slip_raw`
 * writer.  For each present (station, satellite): pick the primary dual-frequency
 * pair (the two lowest distinct bands whose phase and matching code "C"+tail are
 * both present in `raw_observed`), then form MW, GF (= L1 − L2) and the
 * first-signal phase-minus-code, all in metres, plus the per-band RINEX LLI.  The
 * speed of light cancels out of MW and GF, so wavelengths alone suffice
 * (FDMA-correct, since they are looked up by satellite id).
 *
 * @param modeled      Per-station modeled observations (read-only).
 * @param present      Per-station PASS-A presence (absent stations produce no rows).
 * @param n_sta_total  Station count (the pivot is included, for parity with the
 *                     original trace).
 * @param utils        Wavelength provider (non-const: PppUtils caches internally).
 * @param[out] out     Reset by the caller first (@ref RawSlipEpoch::reset); rows are
 *                     appended to `out.rows[i]` in `modeled[i]` order, and
 *                     `out.computed` is set.
 * @param per_signal   Also fill @ref RawSlipObs::sigs with every phase signal that
 *                     has a matching code and a resolvable wavelength (card GRS).
 *                     Default false: the row is byte-identical to the pre-card one
 *                     and nothing extra is allocated.
 * @param tracker_modeled_first
 *                     Take each band's signal from the lookup @p latch when that
 *                     signal also has a raw phase and a matching raw code this
 *                     epoch, and otherwise apply the lexical rule on that band
 *                     (card GRK, @c network.cycle_slip.tracker_modeled_first; the
 *                     call site also sets it under @c network.roster_tracker).  The
 *                     two bands of the pair are the two lowest distinct bands
 *                     either way.  Default false: the lexical rule alone,
 *                     byte-identical.
 * @param cnt          Optional run-end counters.  When non-null and @p latch is
 *                     bound, @ref SlipQcCounters::gap_tracker_key_mismatch is
 *                     accumulated in both states of @p tracker_modeled_first, one
 *                     per (station, satellite, epoch, band) cell whose tracker
 *                     signal differs from the latched one.  Default null, so every
 *                     pre-card caller compiles unchanged.
 * @param latch        The per-station canonical phase lookup (card GRK; see
 *                     @ref TrackerLatchLookup).  Null (the default) leaves the
 *                     tracker on the lexical rule and the mismatch counter at 0
 *                     even with the knob armed, so an off run whose counter reads 0
 *                     on a mixed-signal network points to a lookup that was never
 *                     bound (standing invariant #4).
 */
void networkComputeRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  PppUtils& utils,
                                  RawSlipEpoch& out,
                                  bool per_signal = false,
                                  bool tracker_modeled_first = false,
                                  SlipQcCounters* cnt = nullptr,
                                  const TrackerLatchLookup* latch = nullptr);

/**
 * @brief Adjudicate this epoch's raw steps, update the per-station empirical
 *        scales and the card L credibility state, and collect card G arc
 *        resumptions.  Serial and station-ordered, so deterministic.
 *
 * Does nothing unless `ep.computed` is set.  Order of operations per station (the
 * pool-arm pattern):
 *   1. Form ΔMW / ΔGF against the immediately adjacent processed epoch only.
 *   2. Threshold each satellite with the pre-update scale (cold start: this
 *      epoch's own robust median), so the scale never absorbs the event it
 *      must catch.
 *   3. Fold the clean survivors' robust median into the EWMA.
 *   4. Fold this epoch's LLI-set fraction into the credibility EWMA and apply
 *      the two-state hysteresis.
 *   5. Record non-adjacent reappearances as @ref GapResumption (when
 *      @ref SlipQcParams::gap_instrument or @ref SlipQcParams::gap_reentry is
 *      armed).
 *   5b. Record non-adjacent reappearances of an individual signal on a satellite
 *      whose row survived, as @ref SigResumption (card GRS; additionally needs
 *      @ref SlipQcParams::gap_reentry_per_signal).  A satellite that already
 *      produced a row-level resumption at (5) is skipped, so the two tiers never
 *      count the same break twice.
 *   6. Store this epoch's rows in `prev` and, under the arming of (5b), this
 *      epoch's per-signal levels in `sig_prev`.  Entries of satellites and
 *      signals absent this epoch keep their last-seen values, which (5) and (5b)
 *      difference against.
 *
 * @param modeled     Per-station modeled observations (LLI fraction denominator).
 * @param present     Per-station PASS-A presence.
 * @param n_sta_total Station count.
 * @param epoch_idx   Monotonic processed-epoch index (gap arithmetic unit).
 * @param p           Knob snapshot.
 * @param hist        Per-station cross-epoch state; mutated (resized when it has
 *                    fewer entries than `ep.rows`).
 * @param[in,out] ep  This epoch's buffer: `rows` and `computed` read, everything
 *                    else filled.
 * @param cnt         Run-end counters; accumulated into.
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

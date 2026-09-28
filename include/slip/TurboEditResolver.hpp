#pragma once

/**
 * @file TurboEditResolver.hpp
 * @brief Melbourne-Wübbena and geometry-free joint cycle-slip repair resolver
 *        (Blewitt TurboEdit), shared by the post-processing and real-time paths.
 *
 * @par Motivation
 * The Melbourne-Wübbena (MW) wide-lane combination measures only the wide-lane
 * difference @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$: one equation in two
 * unknowns.  For a satellite tracked on three or more frequencies,
 * @c CycleSlipManager cross-checks overlapping MW pairs to isolate the slipped
 * signal.  In the dual-frequency case MW alone cannot attribute a slip to one
 * signal, and the only safe response is a full ambiguity reset, which discards
 * the converged ambiguity precision.  MW also does not see the
 * @f$\Delta N_1=\Delta N_2@f$ case, where the wide-lane is unchanged.
 *
 * TurboEdit adds the geometry-free (GF) phase combination as the second
 * equation:
 * @f[
 *    \Delta N_{WL}  = \Delta N_1 - \Delta N_2              \quad\text{(MW, integer)}
 * @f]
 * @f[
 *    \Delta\Phi_{GF} = \lambda_1\,\Delta N_1 - \lambda_2\,\Delta N_2
 *                                                          \quad\text{(GF jump, metres)}
 * @f]
 * solved for the per-signal integers
 * @f[
 *    \Delta N_2 = \frac{\Delta\Phi_{GF} - \lambda_1\,\Delta N_{WL}}{\lambda_1-\lambda_2},
 *    \qquad \Delta N_1 = \Delta N_2 + \Delta N_{WL}.
 * @f]
 * @f$\Delta\Phi_{GF}@f$ is estimated by the second-order time difference (TD2) of
 * the geometry-free phase, @f$GF(t)-2\,GF(t\!-\!1)+GF(t\!-\!2)@f$, which removes
 * the constant and linear parts of the ionospheric delay.  At the slip epoch it
 * equals the slip jump plus phase noise and the remaining ionospheric residual.
 *
 * @par Rate adaptivity (evidence-gated GF trust)
 * TD2 removes the ionospheric delay only to second order over the sampling
 * interval, so the residual grows with the interval: sub-mm at 1 s, typically
 * mm to cm at 30 s under a quiet ionosphere, and beyond the integer-lattice
 * spacing when the ionosphere is active.  The residual is not modelled; it
 * enters the arc's TD2 MAD window and widens every gate derived from it.  GF
 * trust is therefore gated on evidence rather than on the rate: each GF-backed
 * decision requires the measured noise to be small enough to discriminate the
 * integer lattice it decides on.
 *  - Per-signal split (repair): the float @f$\Delta N_2@f$ carries
 *    @f$\sigma_{TD2}/|\lambda_1-\lambda_2|@f$ cycles of noise.  The joint solve
 *    emits integers only when that is at most @c turbo_edit_split_max_sigma
 *    (default 0.2 cycles), so a GF error below 3.5σ (0.7 cycles) cannot be
 *    accepted at a wrong integer, which takes an error of at least
 *    @f$1-@f$@c kMaxFractional = 0.7 cycles.  An MW-flagged pair that fails
 *    the gate falls back to the reset.  A GF-only exceedance on an arc that
 *    cannot split is ignored: an unattributable ionospheric transient is not a
 *    reason to reset, and a real equal slip produces a phase innovation of
 *    @f$|dN\cdot\lambda|\ge 19@f$ cm, which the KF residual check catches.
 *  - Code-event veto (affirmative zero): the affirmative-zero gate
 *    @f$3.5\sigma@f$ is also capped at
 *    @c kVetoZeroGateMaxLambdaFrac @f$\times|\lambda_1-\lambda_2|@f$.  A "the
 *    phase did not move" bound wider than half the smallest GF-visible slip
 *    quantum cannot tell stillness from a small slip, so the veto stands down
 *    and the MW reset applies.
 *  - Cross-satellite IVI: unchanged; its threshold is lattice-scale
 *    (about @f$|\lambda_1-\lambda_2|@f$) and therefore independent of the rate.
 * At 1 s these gates pass at typical phase noise and reproduce the high-rate
 * behaviour.  At 30 s (daily RINEX) they pass under a quiet ionosphere, where
 * large MW-detected slips are split and repaired, and they stand down as
 * ionospheric activity rises, so flagged satellites fall back to a reset.  A
 * one-time advisory log line (and @c EpochResolution::rate_ok) still reports
 * intervals above @c kHighRateMaxIntervalS.  See @c project_vertical_instability_solidtide /
 * the @c GfSlipDetector banner for the ionospheric-contamination rationale.
 *
 * @par Separation of concerns
 * This class is a pure resolver: it owns the per-arc GF jump history and the
 * MW+GF arithmetic, and returns integer per-signal repairs plus a set of
 * satellites to reset.  It does not mutate filter state and does not know about
 * BSSD framing; the caller applies the integers in its own state convention.
 * The post-processing @c CycleSlipManager feeds them through its BSSD-aware
 * repair and pivot-handover logic; @c RealTimeEngine applies a
 * covariance-preserving additive shift for non-pivot satellites and resets
 * otherwise.  The MW wide-lane integers are an input (from @c MwSlipDetector)
 * rather than recomputed here, so the empirically tuned MW detection path
 * remains the single source of @f$\Delta N_{WL}@f$.
 *
 * @par GF-only flags require affirmative MW cleanliness
 * For a pair that the GF jump flagged and MW did not, @f$\Delta N_{WL}=0@f$
 * (the equal-slip split) is assumed only when the caller supplies affirmative
 * evidence that the pair's MW observable was clean this epoch
 * (@c MwSlipDetector::getEpochCleanPairs).  MW silence is not that evidence: a
 * wl-locked arc absorbs a real slip into its Welford statistics without
 * emitting a result (drift demotion fires only epochs later), and an immature
 * arc cannot flag at all.  Splitting those under an assumed
 * @f$\Delta N_{WL}=0@f$ turns unequal slips into confident wrong dual-signal
 * repairs.  The float @f$\Delta N_2@f$ shifts by
 * @f$-\lambda_1/(\lambda_1-\lambda_2)\approx 3.53@f$ cycles per unit of
 * @f$\Delta N_{WL}@f$ (GPS L1/L2), so an even @f$\Delta N_{WL}@f$ lands well
 * inside the fractional acceptance gate (a shift of 7.06 cycles for
 * @f$\Delta N_{WL}=2@f$).  Unaffirmed GF-only flags therefore fall back to the
 * reset path.
 *
 * @par MW-only flags with affirmatively stable GF (code-domain veto)
 * The complementary case: MW flagged a pair but the GF jump is affirmatively
 * zero, that is, |TD2| is within the arc's own measured phase noise
 * (@c kCodeEventZeroSigma × MAD, a tighter bound than the detection gate).  The
 * integer geometry then contradicts every physically plausible slip pair.  For
 * @f$\Delta N_{WL}=k@f$, an integer pair @f$(\Delta N_1,\,\Delta N_1-k)@f$
 * produces the GF jump
 * @f$\Delta\Phi_{GF}=\lambda_2 k-(\lambda_2-\lambda_1)\Delta N_1@f$, which
 * crosses zero at @f$\Delta N_1\approx 4.53\,k@f$ (GPS L1/L2;
 * @f$\approx 3.95\,k@f$ for the L1/L5 family).  Every pair with
 * @f$|\Delta N_1|\le 3@f$ predicts @f$|\Delta\Phi_{GF}|\ge 8@f$ cm (L1/L2;
 * ≥ 6 cm for L1/L5), far above phase noise, so a flat GF leaves only null-line
 * pairs of the (4,3) or (9,7) class: simultaneous multi-cycle slips on both
 * signals, without loss of lock, under a quiet ionosphere.  MW is WL phase
 * minus NL code, so a narrow-lane code excursion (multipath, NLOS transition,
 * DLL re-lock; 1 WL cycle ≈ 0.86 m of NL code on L1/L2) produces this
 * signature directly.
 *
 * Under trusted conditions (a zero gate able to discriminate, see "Rate
 * adaptivity": @f$3.5\sigma@f$ at or below
 * @c kVetoZeroGateMaxLambdaFrac @f$\times|\lambda_1-\lambda_2|@f$; a stable
 * ionosphere by IVI; a mature GF arc; no post-slip cooldown; no LLI on either
 * signal) such an event
 * is classified as a code-domain artifact
 * (@c EpochResolution::code_events).  Both signals are adjudicated clean
 * (reported in @c clean_signals) and the caller must skip the mandated
 * phase reset.  The backstop is the Kalman phase innovation: if the event
 * was in fact a null-line slip, that signal's innovation jumps at this
 * epoch's update by at least 0.73 m (3λ2 on L2 and 4λ1 ≈ 0.76 m on L1 for
 * the (4,3) pair), far beyond any standardized residual gate, and the
 * residual-driven reset corrects it one epoch late.  A wrong veto costs one
 * rejected measurement; a wrongly applied MW reset loses converged ambiguity
 * precision (and AR continuity) at once.  For a BSSD pivot the same backstop
 * exists in SD form: a missed pivot slip disturbs every peer residual
 * coherently and the KF coherent-disturbance handover re-references.  When
 * another pair of the same satellite demands a reset or stages a conflicting
 * integer in the same epoch, the satellite-level reset wins and the code
 * event is withdrawn, so contradictory evidence is never split.
 * Disable via @c estimation.cycle_slip.turbo_edit_mw_only_veto.
 *
 * @par Gap-spanning GF bridge (across-gap attribution)
 * The TD2 chain needs three consecutive epochs, so any interruption (a
 * per-signal dropout or a full receiver gap) re-seeds the chain, and the
 * post-gap TD2 cannot see what happened across the gap: a re-lock slip's GF
 * level jump falls between the two chains, and GF appears stable on both
 * sides.  The MW arc spans short dropouts, so its across-gap flag arrives with
 * no GF evidence to adjudicate it, and the caller resets both signals,
 * including the one that held lock and a dropped signal whose ambiguity the
 * state side already pruned and reseeded on return.
 *
 * The bridge addresses this.  When a chain break spans at most
 * @c estimation.cycle_slip.turbo_edit_max_gap_s and the arc had at least two
 * epochs, the pre-gap GF level, its timestamp, the arc's EWMA GF rate and the
 * pre-gap epoch spacing are stashed, and the TD2 MAD calibration window is
 * kept (gap length does not change arc noise).  MW-flagged pairs, and pairs
 * with a pending WL-settle deferral, are then evaluated against the
 * drift-corrected across-gap jump @f$\Delta GF_{gap} = GF_{now} - GF_{ref} -
 * \dot{GF}\,\Delta t@f$ instead of a TD2.  This applies during the post-gap
 * warm-up and also after the chain has a TD2 again, because the MW locked-arc
 * TD-confirmation path delivers its flag one or more epochs late and the
 * rebuilt chain's TD2 does not contain the jump.
 *  - Affirmatively zero (within the code-event gate widened by the random-walk
 *    factor @f$\sqrt{\Delta t/dt_0}@f$, the arc's own per-interval variability
 *    extrapolated over the gap): a code-domain artifact (re-acquisition DLL
 *    transient), as in the MW-only veto.
 *  - Otherwise the joint solve runs with the across-gap jump.  The WL integer
 *    comes from the MW result when MW published one.  The locked-arc
 *    TD-confirmation path withholds its integer by contract, so otherwise the
 *    candidates are round(f) ± @c kBridgeKWindow around the float estimate
 *    @c MwSlipResult::slip_wl_float, or, on a WL-settle deferral epoch, the
 *    rounded settled across-gap estimate.  A candidate must pass the
 *    fractional gate, predict a GF jump distinguishable from zero, and not
 *    require a nonzero integer on a signal that a continuous, unflagged
 *    sibling pair affirms as unslipped; exactly one candidate must remain.
 *    The GF equation alone is degenerate along the null-line lattice
 *    (+0.735 m fits both (0,−3) and (9,4) at mm level; only k separates
 *    them).  A candidate set that is not unique defers the solve (WL-settle
 *    deferral, when enabled) or falls back to a reset.
 *  - The accepted pair is dispatched per signal by observation-domain
 *    continuity, tracked per signal across epochs.  A signal observed at
 *    every epoch through the gap kept its ambiguity (a full receiver gap
 *    prunes nothing), so its nonzero dN is a real integer repair.  A signal
 *    that missed an epoch was pruned and reseeded on return
 *    (prune-on-first-miss), so its nonzero dN is moot: the fresh seed has
 *    already absorbed it.  Moot signals are reported in
 *    @c EpochResolution::moot_signals so that the caller suppresses the
 *    mandate with no state action.  Signals with dN = 0 are adjudicated
 *    clean as usual.
 *  - Trust gates follow the veto: a stable ionosphere by IVI, no LLI, a
 *    mature preserved MAD window, and the rate-adaptive gates evaluated with
 *    the gap-widened σ.  An ambiguous or failed solve falls back to the reset,
 *    and an untrusted bridge leaves the mandate to the caller, as without the
 *    bridge.
 * A bridge is consumed by its first adjudication that takes an action.  It
 * expires once the time since the pre-gap level exceeds
 * @c turbo_edit_max_gap_s (extended by @c turbo_edit_wl_settle_max_s while a
 * WL-settle deferral waits) and is discarded with the arc by an external
 * reset.  While either signal carries an LLI the bridge is untrusted (a
 * receiver-declared loss of lock voids the level reference).
 *
 * @note The GF combination arithmetic here is self-contained.  It mirrors the
 *       staged @c GfSlipDetector and the active @c IonoVariabilityMonitor;
 *       moving the three onto one shared GF-combination helper is a possible
 *       later cleanup and is out of scope for this component.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"   // MwSlipResult (input)
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class TurboEditResolver
 * @brief Resolves dual-frequency cycle slips into per-signal integer repairs by
 *        pairing the MW wide-lane integer with the geometry-free phase jump.
 */
class TurboEditResolver {
public:
    /**
     * @brief A confident per-(satellite, signal) integer slip repair.
     *
     * @c dN is the zero-differenced integer cycle slip in @em this signal's
     * cycles; the caller applies @c dN·λ as an additive, covariance-preserving
     * shift to the corresponding carrier-phase ambiguity (BSSD pivots excepted).
     */
    struct SignalRepair {
        std::string satellite; ///< Satellite identifier (e.g. "G01").
        std::string signal;    ///< Signal identifier (e.g. "L1C").
        int    dN{ 0 };        ///< Integer slip in this signal's cycles (never 0 in emitted repairs).
        double lambda{ 0.0 };  ///< Signal wavelength (m).
        double frac{ 0.0 };    ///< Rounding residual of the float estimate (0 = exact, ≤0.5 worst).
    };

    /**
     * @brief An MW-flagged pair adjudicated as a code-domain artifact.
     *
     * MW flagged the pair but the geometry-free phase was affirmatively
     * stable under trusted conditions (see the file-level "code-domain
     * veto" note), so the event is in the code observable, not the phase.
     * The caller must not reset the pair's phase ambiguities; the KF
     * phase-innovation (residual) gate is the contracted backstop for the
     * remaining null-line-pair risk.  Both signals also appear in
     * @c EpochResolution::clean_signals.  Never emitted for a satellite
     * that ends the epoch in @c reset_sats (conflicting evidence on
     * another pair withdraws the event).
     */
    struct CodeEvent {
        std::string satellite;   ///< Satellite identifier (e.g. "G01").
        std::string sig1;        ///< Higher-frequency phase signal of the MW pair.
        std::string sig2;        ///< Lower-frequency phase signal of the MW pair.
        int    dN_wl{ 0 };       ///< MW wide-lane integer reported for the flag (0 if unknown).
        bool   wl_known{ true }; ///< False when MW flagged without a usable integer.
        double td2_m{ 0.0 };     ///< Measured GF jump (m): the TD2, or the across-gap jump for a bridged event.
        double zero_gate_m{ 0.0 }; ///< Affirmative-zero bound used (kCodeEventZeroSigma × arc MAD, widened by √(Δt/dt₀) when bridged; m).
    };

    /**
     * @brief Per-epoch resolution result.
     */
    struct EpochResolution {
        std::vector<SignalRepair> repairs;   ///< Confident per-signal integer repairs to apply.
        /// Per-satellite signals adjudicated as @c dN==0 (not slipped): the
        /// zero-side partners of the emitted repairs and the signals of code
        /// events.  Lets the caller distinguish "verified clean" from "never
        /// evaluated" when arbitrating its own reset mandates against this
        /// resolution: a mandate on an unevaluated signal must still be
        /// honoured, and one on an adjudicated-clean signal must not discard
        /// that signal's converged precision.
        std::map<std::string, std::set<std::string>> clean_signals;
        /// MW-flagged pairs adjudicated as code-domain artifacts (GF
        /// affirmatively stable); the caller must skip the phase reset
        /// for these pairs.  Disjoint from @c reset_sats by construction.
        std::vector<CodeEvent>    code_events;
        /// Per-satellite signals whose across-gap slip the gap bridge
        /// attributed and adjudicated moot: the signal missed at least one
        /// epoch during the bridged gap, so its ambiguity was pruned and
        /// reseeded on return, the slip is already absorbed, and a further
        /// reset would only discard the new seed.  The caller must suppress
        /// its reset mandates with no state action (same handling as
        /// @c clean_signals and code-event signals).  Disjoint from
        /// @c reset_sats by construction (withdrawn like code events).
        std::map<std::string, std::set<std::string>> moot_signals;
        /// Satellites whose bridged adjudication is deferred until the
        /// across-gap MW wide-lane estimate settles (the WL-settle
        /// deferral; see @c CycleSlipConfig::turbo_edit_wl_settle_max_s).
        /// The instantaneous re-acquisition float is biased by NL code by
        /// up to several cycles, and two integer candidates separated by a
        /// null-line pair fit the GF jump at mm level.  Picking one would
        /// produce a confident wrong repair, so the resolver waits until
        /// the post-restart MW Hatch mean can be rounded with confidence.
        /// While a satellite is listed, the caller must keep its soft-LLI
        /// holds alive (measurements excluded, states protected from
        /// pruning) and suppress its MW reset mandates.  The deferral always
        /// ends in a later epoch's repair, reset or timeout reset.
        /// Disjoint from @c reset_sats and the repair set by construction.
        std::set<std::string>     pending_sats;
        std::set<std::string>     reset_sats; ///< Satellites flagged but unresolved; the caller should reset them.
        bool   iono_stable{ true };          ///< False ⇒ GF jumps untrusted this epoch (IVI gate tripped).
        double max_ivi{ 0.0 };               ///< Worst-case cross-satellite IVI this epoch (m).
        /// Advisory only: false ⇒ the sampling interval exceeded the nominal
        /// high-rate regime (@c kHighRateMaxIntervalS).  No decision is
        /// gated on this; GF trust scales with the evidence of each arc
        /// (see the header's "Rate adaptivity" note).
        bool   rate_ok{ true };
    };

    /**
     * @brief Construct the resolver.
     * @param config      Processing configuration (elevation mask, etc.).
     * @param log_manager Logging facility.
     */
    TurboEditResolver(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Provide satellite BIA corrections.
     *
     * The GF combination is bias-free in its time-difference, so this is a
     * no-op today; retained for interface parity with the detectors and for
     * forward compatibility.
     *
     * @param corrections Bias source (may be nullptr); owned externally.
     */
    void setCorrections(const GnssCorrections* corrections) { corrections_ = corrections; }

    /**
     * @brief Resolve cycle slips for one epoch.
     *
     * Call it every epoch, even when @p mw_results is empty: it rolls the GF
     * jump arcs forward, and a skipped epoch loses the GF history needed for
     * the next jump estimate.
     *
     * @param mw_results   MW slip results from @c MwSlipDetector::detectLargeMwResets
     *                     for this epoch (the source of @f$\Delta N_{WL}@f$).
     * @param mw_clean_pairs Arc keys ("SAT_SIG1_SIG2") whose MW observable was
     *                     affirmatively clean this epoch, from
     *                     @c MwSlipDetector::getEpochCleanPairs().  Gates the
     *                     GF-only equal-slip split (see the file-level
     *                     "GF-only flags require affirmative MW cleanliness"
     *                     note); pairs absent from this set fall back to
     *                     reset when GF-only flagged.
     * @param observations Modeled observations for this epoch (raw carrier phase, cycles).
     * @param interval_s   Effective sampling interval (s); ≤0 means unknown (the
     *                     rate warning is then suppressed).
     * @param wl_gap_evidence Settling across-gap WL estimates per arc key,
     *                     from @c MwSlipDetector::getAcrossGapWlEvidence.
     *                     The WL-settle deferral uses them to re-run a
     *                     deferred bridged solve once the post-restart MW
     *                     mean can be rounded with confidence.  Empty
     *                     disables re-evaluation (pending satellites then
     *                     run to the deferral timeout).
     * @return Per-signal integer repairs and the reset-fallback set.
     */
    EpochResolution resolve(const std::vector<MwSlipResult>& mw_results,
                            const std::set<std::string>& mw_clean_pairs,
                            const std::vector<PppObservation>& observations,
                            double interval_s,
                            const std::map<std::string,
                                           MwSlipDetector::AcrossGapWlEvidence>&
                                wl_gap_evidence = {});

    /**
     * @brief Mark a (satellite, signal) as held across a receiver LLI gap.
     *
     * Soft-LLI bridge support (see @c GnssConfig::CycleSlipConfig
     * @c turbo_edit_bridge_lli).  When the caller holds a satellite's
     * ambiguity across a short LLI gap instead of resetting it, it calls
     * this so that the gap-bridge adjudication treats the signal as
     * continuous (a real integer repair) rather than moot (pruned and
     * reseeded); the moot case assumes the state side reseeded the
     * ambiguity, which the hold prevents.  The flag is one-shot: the
     * bridge consumes it when it acts on the pair (repair, reset or code
     * event), and an arc reset for the satellite or for this (satellite,
     * signal) clears it.
     *
     * @param satellite Satellite identifier (e.g. "G03").
     * @param signal    Signal identifier (e.g. "L2W").
     */
    void markHeldAcrossLli(const std::string& satellite, const std::string& signal);

    /**
     * @brief Clear all GF arc state for a satellite (every signal pair).
     * @param satellite Satellite identifier (e.g. "G01").
     */
    void resetSatellite(const std::string& satellite);

    /**
     * @brief Clear GF arc state for pairs involving a specific (satellite, signal).
     * @param satellite Satellite identifier (e.g. "G01").
     * @param signal    Signal identifier (e.g. "L1C").
     */
    void resetSatelliteSignal(const std::string& satellite, const std::string& signal);

    /**
     * @brief Drop all GF arc state (hard-gap restart).
     */
    void resetAll();

    /**
     * @brief Override the inter-epoch gap tolerance before a GF arc is reset.
     * @param seconds New gap tolerance; values ≤ 0 are ignored.
     */
    void setMaxGapSeconds(double seconds);

    /**
     * @brief Enable CSV tracing of the per-pair joint solve.
     * @param log_dir Directory in which to create @c turbo_edit_trace.csv.
     * @return true if the trace file was opened.
     */
    bool enableTracing(const std::filesystem::path& log_dir);

    /// @brief Whether tracing is currently active.
    bool isTracingEnabled() const { return trace_file_.is_open(); }

private:
    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;
    const GnssCorrections* corrections_{ nullptr };

    /// Effective inter-epoch gap tolerance (s); overridable via @c setMaxGapSeconds.
    double max_gap_seconds_{ kDefaultMaxGapSeconds };

    /// One-shot guard so the coarse-rate warning is logged only once per run.
    bool rate_warned_{ false };

    /// Continuous GPS seconds of the previous epoch that carried observations.
    /// The 2nd-order time difference requires three consecutive epochs, so an
    /// arc whose last update predates this (it missed the previous epoch in a
    /// per-signal dropout) is non-consecutive and is restarted, regardless of
    /// the absolute @c max_gap_seconds_ tolerance.  0 until the first epoch.
    double prev_epoch_s_{ 0.0 };

    /**
     * @brief A dual-frequency carrier-phase pair, ordered higher-frequency first.
     */
    struct PhasePair {
        std::string phase1;  ///< Higher-frequency phase signal (e.g. "L1C").
        std::string phase2;  ///< Lower-frequency phase signal (e.g. "L2W").
    };

    /**
     * @brief Per-(satellite, phase-pair) geometry-free jump arc.
     */
    struct GfArc {
        double prev_gf{ 0.0 };           ///< GF at epoch t-1 (m).
        double prev_prev_gf{ 0.0 };      ///< GF at epoch t-2 (m).
        double last_epoch_s{ 0.0 };      ///< Continuous GPS seconds of last update.
        std::size_t count{ 0 };          ///< Epochs accumulated in this arc.
        /// Recent TD2 values (m) for the MAD threshold.  This is calibration,
        /// not chain state: the TD2 noise level is a slowly varying property
        /// of the arc (elevation, tracking, multipath), so the window
        /// survives the short-gap partial reset (gap ≤ @c max_gap_seconds_),
        /// a bridged gap, and the slip reset.  A level jump does not change
        /// the noise, and the window holds only clean (non-exceeding) TD2s.
        /// Only an absolute-gap reset or an external (KF-driven) arc erase
        /// clears it.
        std::deque<double> diff_window;
        std::size_t cooldown_remaining{ 0 }; ///< Anti-echo cooldown after a slip (epochs).

        // --- Gap bridge (see the file-level "gap-spanning GF bridge" note) --
        /// EWMA of the GF rate (m/s) over consecutive epochs; the
        /// drift-correction source for the across-gap jump.  NaN until two
        /// consecutive epochs exist.  It survives a chain re-seed only through
        /// the stash below (a fresh chain learns its own rate again).
        double rate_ewma_mps{ std::numeric_limits<double>::quiet_NaN() };
        /// Epoch spacing (s) of the arc's last consecutive update: the
        /// random-walk reference dt₀ for the bridge gate widening.
        double last_spacing_s{ 0.0 };
        /// True while a stashed pre-gap level is available for bridging.
        bool   bridge_valid{ false };
        double bridge_gf{ 0.0 };        ///< Last pre-gap GF level (m).
        double bridge_epoch_s{ 0.0 };   ///< Its continuous GPS seconds.
        double bridge_rate_mps{ std::numeric_limits<double>::quiet_NaN() }; ///< Pre-gap EWMA rate (m/s).
        double bridge_dt0_s{ 0.0 };     ///< Pre-gap epoch spacing (s) for the √(Δt/dt₀) widening.
        /// WL-settle deferral: true while the bridged solve for this pair
        /// waits for a settled across-gap MW estimate (the float at
        /// re-acquisition was biased by code and the candidate set was
        /// ambiguous).  Keeps the bridge stash alive past the chain rebuild;
        /// ends in a later repair, reset or timeout.
        bool   wl_settle_pending{ false };
        double wl_settle_start_s{ 0.0 };  ///< Deferral start (continuous GPS s).
    };

    /// Per-(satellite + phase-pair) GF jump arcs.  Key: "G01_L1C_L2W".
    std::map<std::string, GfArc> gf_arcs_;

    /// Cross-satellite TD2 σ per (constellation × band-pair) group, keyed
    /// "G_L1_L2" (tracking attribute collapsed, mirroring the MW detector's
    /// cross-sat pool).  EWMA-smoothed (@c kXsatEwmaAlpha) and
    /// zenith-referenced (the pool normalizes each TD2 by 1/sin(el);
    /// @c xsatSigmaFloor re-expands on use).  Used only while an arc's own
    /// MAD window is immature (warm-up only, the same contract as the MW
    /// detector), so fresh arcs (cold start, post-gap, post-slip) get a
    /// statistically grounded gate from their first TD2 instead of a NaN.
    /// Lags one epoch by construction (finalized after all pairs are
    /// processed).  On quiet data k×σ_xsat is below @c kMinThresholdM and
    /// the quantum floor governs; the pool raises fresh-arc gates during
    /// ionospheric activity or on noisy receivers, where the bare floor
    /// would raise false flags and trigger resets.
    std::map<std::string, double> xsat_sigma_group_;
    /// Constellation-tier fallback for sparse band-pair groups ("G").
    std::map<std::string, double> xsat_sigma_cons_;
    /// Global (non-GLONASS) tier-3 fallback, mirroring the MW cascade: a
    /// constellation contributing fewer than @c kMinXsatSamples poolable
    /// TD2s per epoch (1-2 visible sats) never populates its own tiers and
    /// would otherwise sit at the bare quantum floor for the whole run.
    /// GLONASS is excluded (its FDMA inter-channel noise is least
    /// comparable), as are narrow-spacing intra-E5/B2-family pairs (their
    /// sub-mm TD2 noise would pull the coarse tiers an order of magnitude low).
    double xsat_sigma_global_{ std::numeric_limits<double>::quiet_NaN() };
    /// Per-epoch staging pool (zenith-normalized TD2 of non-exceeding pairs),
    /// finalized into the tiers at the end of each resolve() and cleared.
    std::map<std::string, std::vector<double>> xsat_devs_;

    // --- Tuning constants ------------------------------------------------

    /// Rolling window length for the MAD-based GF-only detection threshold.
    static constexpr std::size_t kGfWindowSize = 30;
    /// Minimum TD2 samples before the adaptive MAD threshold is trusted.
    static constexpr std::size_t kMinEpochsForAdaptive = 5;
    /// kσ multiplier for the GF-only (blind-spot) detection threshold, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_sigma_multiplier (default 6.0).
    double sigma_multiplier_;
    /// Split-capability gate: maximum 1σ uncertainty of the float per-signal
    /// integer, in cycles of the @f$\lambda_1-\lambda_2@f$ lattice, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_split_max_sigma
    /// (default 0.2).  The joint solve emits integers only when
    /// @f$\sigma_{TD2}\le@f$ this @f$\times|\lambda_1-\lambda_2|@f$.  This is
    /// what makes the resolver rate-adaptive (see the header's "Rate
    /// adaptivity" note): millimetric 1 s noise passes easily, and the
    /// coarse-rate ionospheric residual passes only while the ionosphere is
    /// quiet.
    double split_max_sigma_;
    /// MW-only code-domain veto enable, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_mw_only_veto (default true).
    /// See the file-level "code-domain veto" note.
    bool mw_only_veto_{ true };
    /// Gap-bridge cap (s), from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_max_gap_s (default 15;
    /// 0 disables).  See the file-level "gap-spanning GF bridge" note.
    double bridge_max_gap_s_{ 15.0 };
    /// WL-settle deferral cap (s), from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_wl_settle_max_s
    /// (default 45; 0 disables).  Effective only with the soft-LLI bridge
    /// (@c turbo_edit_bridge_lli): the deferral relies on the caller's
    /// holds to keep the affected phase measurements out of the filter
    /// while it waits.
    double wl_settle_max_s_{ 45.0 };
    /// Whether the WL-settle deferral may engage (bridge_lli && cap > 0).
    bool   wl_settle_enabled_{ false };
    /// Half-width of the float-derived WL integer candidate window
    /// (round(float) ± this).  The re-acquisition float is biased by NL code
    /// by up to a few cycles (2.25 WL cycles observed in field data), so a
    /// ±1 window can exclude the true integer and leave a wrong candidate
    /// as the only one inside the window.  ±3 admits the bias; ambiguity
    /// among the admitted candidates is resolved by the sibling-pair
    /// zero-affirmation constraint or the WL-settle deferral, not by
    /// truncating the window.
    static constexpr int kBridgeKWindow = 3;

    /// Observation-domain per-(sat, signal) phase-continuity tracking for the
    /// gap bridge's moot-vs-repair dispatch.  Key "G03_L2W".
    /// @c sig_continuous_since_s_ holds the start of the signal's current
    /// unbroken run of observed epochs.  A signal whose run started at or
    /// before a bridge's reference epoch was observed at every epoch through
    /// the gap (a full receiver gap breaks nothing, since no epochs
    /// occurred), so its ambiguity survived.  A signal that missed an epoch
    /// that did occur was pruned and reseeded by the state side
    /// (prune-on-first-miss).  Below-elevation-mask epochs count as missing
    /// (those observations are excluded from the filter too).  Cleared only
    /// by @c resetAll: continuity is an observation-domain fact, independent
    /// of filter resets.
    std::map<std::string, double> sig_last_seen_s_;
    std::map<std::string, double> sig_continuous_since_s_;
    /// (sat, signal) keys ("G03_L2W") the caller holds across a receiver LLI
    /// gap (@c markHeldAcrossLli).  The gap-bridge dispatch adjudicates a
    /// held signal as continuous (integer repair) rather than moot: the
    /// caller's hold preserves the pre-gap ambiguity value, so the across-gap
    /// integer is a real repair, not a slip already absorbed by a reseed.
    /// One-shot: erased when the bridge acts on the pair, and on any arc
    /// reset touching the (sat, signal).  Empty unless @c turbo_edit_bridge_lli.
    std::set<std::string> held_across_lli_;
    /// Floor on the GF-only detection threshold (m).  Below |λ1−λ2| (~0.054 m
    /// for GPS L1/L2) so a ±1-cycle equal slip (MW's blind spot) is detectable.
    static constexpr double kMinThresholdM = 0.04;
    /// Maximum rounding residual of the float ΔN2 to accept the integer split.
    static constexpr double kMaxFractional = 0.30;
    /// kσ multiplier for the code-event affirmative-zero test: an MW-flagged
    /// pair is adjudicated a code-domain artifact only when |TD2| ≤ this ×
    /// the arc's own TD2 MAD-σ (≈ 99.95% two-sided coverage for clean phase
    /// noise).  Tighter than the detection gate (@c sigma_multiplier_,
    /// default 6.0): affirming that the phase did not move needs more
    /// discrimination than flagging an outlier, and any real GF movement
    /// beyond this bound must reach the joint solve instead (e.g. a real
    /// (4,3) L1/L2 pair at 2.9 cm against mm-level σ).  Purely empirical:
    /// the bound scales with the arc's own measured noise and has no
    /// absolute floor, so a noisy arc widens it (and the lattice cap
    /// @c kVetoZeroGateMaxLambdaFrac then disables the veto), and a quiet
    /// arc keeps the veto from absorbing repairable small pairs.
    static constexpr double kCodeEventZeroSigma = 3.5;
    /// Minimum TD2 MAD-window samples before the affirmative-zero test is
    /// allowed.  Stricter than @c kMinEpochsForAdaptive (5): a 5-sample MAD
    /// has too much estimator variance to support a decision that preserves
    /// state at the claimed ~3.5σ coverage.  A detection error risks only a
    /// false flag; an affirmation error protects a slipped ambiguity.  Below
    /// this count the veto stands down and the MW mandate resets as usual.
    static constexpr std::size_t kCodeEventMinMadSamples = 10;
    /// Discrimination cap on the code-event affirmative-zero gate, as a
    /// fraction of the pair's @f$|\lambda_1-\lambda_2|@f$ (the smallest
    /// GF-visible slip quantum, the dN1=dN2=±1 pair).  A zero gate wider
    /// than this cannot tell "the phase did not move" from a small slip, or
    /// from the near-null-line small pairs ((4,3)/(5,4) L1/L2 at 2.9/2.5 cm)
    /// that the veto must pass on to the joint solve, so the veto stands
    /// down and the MW mandate governs.  At 1 s the mm-scale MAD keeps the
    /// gate far below this cap, so high-rate behaviour is unchanged; the cap
    /// binds at coarse rates, where the ionosphere-inflated MAD would
    /// otherwise let the veto absorb real small pairs.
    static constexpr double kVetoZeroGateMaxLambdaFrac = 0.5;
    /// EWMA smoothing factor for the per-arc GF rate (m/s) that drift-corrects
    /// the across-gap bridge jump.  A single TD1 carries √2·σ_GF of noise;
    /// projected over a 7–15 s gap it would add more error than the
    /// quiet-ionosphere drift it corrects.  The EWMA (noise ≈ √(α/(2−α)) ≈
    /// 0.23× a single TD1 at α=0.1) tracks the slowly varying ionospheric
    /// trend without that amplification.
    static constexpr double kBridgeRateAlpha = 0.1;
    /// Default inter-epoch gap (s) before a GF arc is reset.
    static constexpr double kDefaultMaxGapSeconds = 120.0;
    /// Anti-echo cooldown after a slip (epochs): the TD2 echo of a prior event
    /// must not retrigger a GF-only detection.
    static constexpr std::size_t kCooldownEpochs = 3;
    /// Cross-satellite IVI threshold (m): above this the ionosphere is deemed
    /// active and GF jumps are not trusted.  Matches @c GfSlipDetector.
    static constexpr double kIviThreshold = 0.06;
    /// Minimum cross-sat pool size before its MAD is statistically meaningful;
    /// below this the previous EWMA value persists.  Matches the MW detector.
    static constexpr std::size_t kMinXsatSamples = 5;
    /// EWMA smoothing factor for the cross-sat TD2 σ tiers (first finite MAD
    /// bootstraps).  Matches the MW detector's default cross-sat alpha.
    static constexpr double kXsatEwmaAlpha = 0.1;
    /// Sanity bound on a resolved per-signal integer slip.
    static constexpr int kMaxSlipCycles = 1000;
    /// Sampling interval (s) above which the one-time coarse-rate advisory is
    /// logged and @c EpochResolution::rate_ok reports false.  Advisory only:
    /// no decision is gated on it (see the header's "Rate adaptivity" note).
    static constexpr double kHighRateMaxIntervalS = 1.0;

    // --- GF combination helpers (self-contained; see file note) ----------

    /**
     * @brief Collect every dual-frequency carrier-phase pair from an observation,
     *        ordered higher-frequency first.
     *
     * All tracked phase attributes are paired (not one canonical per band) so
     * that whatever (sig1, sig2) the MW detector reported has a matching GF arc.
     */
    std::vector<PhasePair> collectPhasePairs(const PppObservation& obs) const;

    /**
     * @brief Geometry-free phase observable @f$L_1-L_2@f$ in metres.
     * @return GF value (m), or NaN on missing/invalid input.
     */
    double computeGfMeters(const PppObservation& obs,
                           const std::string& sig1,
                           const std::string& sig2) const;

    /// @brief MAD-derived Gaussian-equivalent sigma of a TD2 window (NaN if immature).
    static double computeMadSigma(const std::deque<double>& window);

    /**
     * @brief Best available TD2 noise σ for an arc (m).
     *
     * The arc's own MAD when the window is mature, else the cross-sat tier
     * floor (elevation re-expanded), else NaN (true cold start).  This is the
     * evidence every rate-adaptive gate is scaled by.
     *
     * @param arc      Arc whose TD2 window is consulted.
     * @param arc_key  "SAT_SIG1_SIG2" key for the cross-sat fallback lookup.
     * @param elev_deg Arc elevation (deg) for the zenith re-expansion.
     * @return σ in meters, or NaN when no estimate exists.
     */
    double effectiveSigma(const GfArc& arc, const std::string& arc_key,
                          double elev_deg) const;

    /**
     * @brief Split-capability gate (see the header's "Rate adaptivity" note).
     *
     * True when the GF evidence is sharp enough to attribute per-signal
     * integers on the @f$\lambda_1-\lambda_2@f$ lattice:
     * @f$\sigma\le@f$ @c split_max_sigma_ @f$\times|denom|@f$.  False on a
     * NaN σ (no evidence is not evidence of sharpness) and on a degenerate
     * pair.
     *
     * @param sigma_m TD2 noise σ in meters (for bridged adjudications, the
     *                gap-widened value).
     * @param denom_m Lattice spacing @f$\lambda_1-\lambda_2@f$ (m).
     */
    bool splitCapable(double sigma_m, double denom_m) const;

    /**
     * @brief Adaptive GF-only detection threshold (m) for an arc.
     *
     * Two regimes, both floored at @c kMinThresholdM and both finite, so
     * the gate is never NaN:
     *
     *  - Mature arc (≥ @c kMinEpochsForAdaptive TD2 samples): kσ × the
     *    arc's own MAD governs.  The cross-sat σ is not consulted here
     *    (warm-up-only contract, as in the MW detector).
     *  - Warm-up arc: kσ × the cross-sat TD2 σ for this arc's
     *    (constellation × band-pair) group (zenith-pooled, EWMA-smoothed,
     *    re-expanded to this arc's elevation via @c xsatSigmaFloor),
     *    resolved through the cascade band-pair → constellation → global
     *    non-GLONASS so that sparse constellations borrow from the broader
     *    population.  The bare @c kMinThresholdM quantum floor gates alone
     *    only at the cold start (first ~2 epochs of a run) and for GLONASS
     *    when its own tiers are thin.
     *
     * On clean arcs the floor governs in both regimes by design.  The
     * smallest physically meaningful GF event is the dN1=dN2=±1 quantum
     * |λ1−λ2| (~5.4 cm GPS L1/L2), so the adaptive terms only raise the
     * gate on noisy arcs or under a disturbed ionosphere; they never lower
     * it on quiet arcs below the scale of a physical slip.
     *
     * @param arc       Arc whose TD2 window is consulted.
     * @param arc_key   "SAT_SIG1_SIG2" key, reduced via @c xsatGroupKey
     *                  for the cross-sat lookup.
     * @param elev_deg  Arc elevation (deg) for the zenith re-expansion.
     * @param out_unfloored Optional; receives the active adaptive term
     *        with no floor applied (mature: kσ·MAD(arc); warm-up:
     *        kσ·σ_xsat; NaN only when neither exists), traced as the
     *        @c thresh_unfloored_m diagnostic column.
     */
    double computeAdaptiveThreshold(const GfArc& arc,
                                    const std::string& arc_key,
                                    double elev_deg,
                                    double* out_unfloored = nullptr) const;

    /**
     * @brief Reduce an arc key to its cross-sat pooling key.
     *
     * "G01_L1C_L2W" → "G_L1_L2": the constellation character plus the
     * 2-character frequency-band prefixes, with the tracking attribute
     * dropped.  Pooling by frequency pair joins L1C/L2W with L1C/L2L and
     * similar, mirroring the MW detector's group key, so that sparse
     * attribute combinations still reach @c kMinXsatSamples.
     *
     * @return Group key, or empty when the arc key is malformed.
     */
    static std::string xsatGroupKey(const std::string& arc_key);

    /**
     * @brief Cross-sat TD2 σ for an arc, resolved band-pair group →
     *        constellation → global non-GLONASS tier, re-expanded to the
     *        arc's elevation.
     * @return σ in meters, or NaN when no tier is populated.
     */
    double xsatSigmaFloor(const std::string& arc_key, double elev_deg) const;

    /**
     * @brief Fold the epoch's staging pool into the EWMA cross-sat tiers
     *        and clear it.  Called at the end of every @c resolve(), so
     *        the floor lags one epoch, like the MW pool.
     */
    void finalizeXsatSigmas();

    /**
     * @brief Cross-satellite ionospheric-variability index for a constellation.
     * @param constellation_td2 Per-arc |TD2| values for one constellation this epoch.
     * @return IVI (m): median(|TD2|) + MAD-sigma(|TD2|); 0 if insufficient data.
     */
    static double computeIvi(const std::vector<double>& constellation_td2);

    /// @brief Build the GF arc key "SAT_SIG1_SIG2".
    static std::string arcKey(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2);

    /// Output stream for the per-pair joint-solve CSV trace.
    mutable std::ofstream trace_file_;
    /// Epoch time captured for tracing.
    GnssTime current_epoch_;
};

} // namespace GnssProcessor
